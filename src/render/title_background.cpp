#include "render/title_background.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <cstring>
#include <exception>
#include <vector>

namespace render {
namespace {

const int kScreenWidth = 960;
const int kScreenHeight = 544;

// opening.dsk's last camera key, where the descent stops and the title screen
// sits. Everything up to it is the animation.
const float kLastCinematicFrame = 209.0f;

// Keep the render target at the Vita panel's native resolution. Playback rate
// is controlled only by temporal sampling: the presentation is locked to 30 Hz
// and advances the cinematic two frames per displayed frame, so a stride of 2
// gives one distinct rasterised image per displayed frame at full speed.
#ifndef PIKMIN_BACKGROUND_STRIDE
#define PIKMIN_BACKGROUND_STRIDE 2
#endif

// The strip is the one real constraint here. At the panel's native 960x544 a
// 32-bit frame is 2.09MB, so sampling the descent at 30 fps would want 219MB of
// texture memory. The background is fully opaque, so the alpha channel is dead
// weight; storing 16-bit halves that to a footprint already shown to fit.
void pack565(const raster::Target& target, std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(target.width) * static_cast<size_t>(target.height) * 2);
  uint16_t* rows = reinterpret_cast<uint16_t*>(out.data());
  for (int y = 0; y < target.height; ++y) {
    const uint8_t* source = target.rgba.data() + static_cast<size_t>(y) * target.width * 4;
    uint16_t* row = rows + static_cast<size_t>(y) * target.width;
    for (int x = 0; x < target.width; ++x) {
      const uint8_t* pixel = source + static_cast<size_t>(x) * 4;
      row[x] = static_cast<uint16_t>(((pixel[0] & 0xF8) << 8) | ((pixel[1] & 0xFC) << 3) |
                                     (pixel[2] >> 3));
    }
  }
}

vita2d_texture* upload_packed(const uint8_t* packed, int width, int height) {
  vita2d_texture* texture = vita2d_create_empty_texture_format(
      static_cast<unsigned>(width), static_cast<unsigned>(height),
      SCE_GXM_TEXTURE_FORMAT_R5G6B5);
  if (texture == nullptr) {
    return nullptr;
  }
  uint8_t* destination = static_cast<uint8_t*>(vita2d_texture_get_datap(texture));
  const unsigned stride = vita2d_texture_get_stride(texture);
  const size_t row_bytes = static_cast<size_t>(width) * 2;
  for (int y = 0; y < height; ++y) {
    std::memcpy(destination + static_cast<size_t>(y) * stride,
                packed + static_cast<size_t>(y) * row_bytes, row_bytes);
  }
  vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                             SCE_GXM_TEXTURE_FILTER_LINEAR);
  return texture;
}

// The strip depends only on the models, the camera and the sampling stride, so
// it is the same on every boot and is worth keeping. Rasterising it takes
// minutes; reading it back takes seconds.
const char* const kCacheDir = "ux0:data/pikmin";
const char* const kCachePath = "ux0:data/pikmin/title_strip.bin";
// Built under a separate name and renamed on completion, so a run interrupted
// half way cannot leave a partial strip that looks finished.
const char* const kCachePartPath = "ux0:data/pikmin/title_strip.part";

const uint32_t kCacheMagic = 0x53545031;
// Bump this whenever a change would rasterise the strip differently, otherwise
// a stale cache keeps being replayed by a build that disagrees with it.
const uint32_t kCacheVersion = 1;

struct CacheHeader {
  uint32_t magic;
  uint32_t version;
  int32_t width;
  int32_t height;
  uint32_t frame_count;
  uint32_t loop_start;
  float frame_stride;
};

// Each read is roughly a megabyte, so taking one frame per displayed frame
// would spend most of the load waiting for vblank instead of reading.
const int kCacheFramesPerStep = 12;

// The Vita gives an application three of its four cores, and a frame splits
// across them cleanly: each worker owns a horizontal band of the target, so no
// two threads write the same pixel and no locking is needed. Both actors are
// drawn in order within a band, which keeps their overlap resolving the same
// way it does single-threaded. tools/band_check confirms a banded frame is
// byte-identical to a whole one.
//
// These are SceKernelThreads rather than std::thread: libstdc++ here reports
// "Enable multithreading to use std::thread" and throws, because it does not
// see the SDK's pthread shim as an active thread implementation.
const int kRasterThreads = 3;

// Affinity is pinned so the bands genuinely land on different cores instead of
// time-slicing one.
const int kBandAffinity[kRasterThreads] = {
    SCE_KERNEL_CPU_MASK_USER_0,
    SCE_KERNEL_CPU_MASK_USER_1,
    SCE_KERNEL_CPU_MASK_USER_2,
};

struct BandJob {
  raster::Target* target;
  const raster::Actor* opening;
  const raster::Actor* logo;
  const Mat4* view_projection;
  float opening_frame;
  float logo_frame;
  int row_begin;
  int row_end;
};

int band_entry(SceSize args, void* argp) {
  (void)args;
  BandJob* job = *static_cast<BandJob**>(argp);
  job->opening->draw(*job->target, *job->view_projection, job->opening_frame, job->row_begin,
                     job->row_end);
  job->logo->draw(*job->target, *job->view_projection, job->logo_frame, job->row_begin,
                  job->row_end);
  return 0;
}

void render_frame(raster::Target& target, const raster::Actor& opening, const raster::Actor& logo,
                  const Mat4& view_projection, float opening_frame, float logo_frame) {
  const int height = target.height;
  BandJob jobs[kRasterThreads];
  SceUID threads[kRasterThreads];
  int started = 0;

  for (int i = 1; i < kRasterThreads; ++i) {
    BandJob& job = jobs[i];
    job.target = &target;
    job.opening = &opening;
    job.logo = &logo;
    job.view_projection = &view_projection;
    job.opening_frame = opening_frame;
    job.logo_frame = logo_frame;
    job.row_begin = height * i / kRasterThreads;
    job.row_end = height * (i + 1) / kRasterThreads;

    threads[started] = sceKernelCreateThread("pikmin_raster_band", band_entry, 0x40, 0x40000, 0,
                                             kBandAffinity[i], nullptr);
    if (threads[started] < 0) {
      // Falling back to drawing this band inline keeps the image correct when
      // a thread cannot be created; only the speed is lost.
      opening.draw(target, view_projection, opening_frame, job.row_begin, job.row_end);
      logo.draw(target, view_projection, logo_frame, job.row_begin, job.row_end);
      continue;
    }
    BandJob* pointer = &job;
    sceKernelStartThread(threads[started], sizeof(BandJob*), &pointer);
    ++started;
  }

  // The calling thread takes the first band rather than idling.
  opening.draw(target, view_projection, opening_frame, 0, height / kRasterThreads);
  logo.draw(target, view_projection, logo_frame, 0, height / kRasterThreads);

  for (int i = 0; i < started; ++i) {
    sceKernelWaitThreadEnd(threads[i], nullptr, nullptr);
    sceKernelDeleteThread(threads[i]);
  }
}

}  // namespace

bool TitleBackground::open_cache() {
  const SceUID fd = sceIoOpen(kCachePath, SCE_O_RDONLY, 0777);
  if (fd < 0) {
    return false;
  }

  CacheHeader header;
  const bool usable = sceIoRead(fd, &header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
                      header.magic == kCacheMagic && header.version == kCacheVersion &&
                      header.width == width_ && header.height == height_ &&
                      header.frame_stride == frame_stride_ && header.frame_count > 0;
  if (!usable) {
    sceIoClose(fd);
    return false;
  }

  cache_read_fd_ = fd;
  cache_remaining_ = header.frame_count;
  expected_frames_ = header.frame_count;
  loop_start_ = header.loop_start;
  frames_.reserve(header.frame_count);
  return true;
}

bool TitleBackground::read_cached_frames(int count) {
  const size_t frame_bytes = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
  packed_.resize(frame_bytes);

  for (int i = 0; i < count && cache_remaining_ > 0; ++i) {
    if (sceIoRead(cache_read_fd_, packed_.data(), frame_bytes) !=
        static_cast<int>(frame_bytes)) {
      // A short read is not fatal: the frames already up play, and the strip
      // gets rasterised again on the next boot.
      cache_remaining_ = 0;
      break;
    }
    vita2d_texture* texture = upload_packed(packed_.data(), width_, height_);
    if (texture == nullptr) {
      error_ = "title background ran out of texture memory";
      cache_remaining_ = 0;
      break;
    }
    frames_.push_back(texture);
    --cache_remaining_;
  }

  if (cache_remaining_ > 0) {
    return true;
  }

  sceIoClose(cache_read_fd_);
  cache_read_fd_ = -1;
  if (loop_start_ >= frames_.size()) {
    // The sway cycle never arrived, so there is nothing to repeat. draw()
    // treats zero as "no loop" and holds on the last frame instead.
    loop_start_ = 0;
  }
  return false;
}

void TitleBackground::finish_cache(bool complete) {
  if (cache_write_fd_ < 0) {
    return;
  }

  if (complete) {
    // The counts are only known now, so the placeholder header written up
    // front is overwritten with the real one.
    CacheHeader header;
    header.magic = kCacheMagic;
    header.version = kCacheVersion;
    header.width = width_;
    header.height = height_;
    header.frame_count = static_cast<uint32_t>(frames_.size());
    header.loop_start = static_cast<uint32_t>(loop_start_);
    header.frame_stride = frame_stride_;
    sceIoLseek(cache_write_fd_, 0, SCE_SEEK_SET);
    sceIoWrite(cache_write_fd_, &header, sizeof(header));
  }

  sceIoClose(cache_write_fd_);
  cache_write_fd_ = -1;

  if (complete) {
    sceIoRemove(kCachePath);
    sceIoRename(kCachePartPath, kCachePath);
  } else {
    sceIoRemove(kCachePartPath);
  }
}

bool TitleBackground::begin(const char* data_root) {
  if (source_ != nullptr || cache_read_fd_ >= 0 || !frames_.empty()) {
    return true;
  }

  width_ = kScreenWidth;
  height_ = kScreenHeight;
  frame_stride_ = static_cast<float>(PIKMIN_BACKGROUND_STRIDE);

  if (open_cache()) {
    return true;
  }

  Source* source = new Source();
  try {
    const std::string root = data_root;
    const std::string opening_base = root + "cinemas/opening/";
    const std::string logo_base = root + "cinemas/titles/";
    source->dsk =
        cine::parse_dsk(cine::read_text_file(opening_base + "opening.dsk"), "opening.dsk");

    // Loaded once and posed per frame; reparsing either model per frame would
    // cost far more than the rasterising does.
    if (!source->opening.load(opening_base + "opening.mod", opening_base + "opening.anm",
                              "opening", error_)) {
      delete source;
      return false;
    }
    if (!source->logo.load(logo_base + "logo.mod", logo_base + "logo.anm", "logo", error_)) {
      delete source;
      return false;
    }
    source->logo.loops = true;
  } catch (const std::exception& exception) {
    error_ = exception.what();
    delete source;
    return false;
  }

  source->frame = 0.0f;
  source->in_sway = false;
  source->sway_period = source->logo.animation.frame_count > 1
                            ? static_cast<float>(source->logo.animation.frame_count - 1)
                            : 0.0f;

  const size_t descent =
      static_cast<size_t>(kLastCinematicFrame / frame_stride_) + 1;
  const size_t sway = static_cast<size_t>(source->sway_period / frame_stride_);
  expected_frames_ = descent + sway;

  source_ = source;
  error_.clear();

  sceIoMkdir(kCacheDir, 0777);
  cache_write_fd_ = sceIoOpen(kCachePartPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (cache_write_fd_ >= 0) {
    // Reserves the header's space; the frame counts are filled in once the
    // strip is finished.
    const CacheHeader placeholder = {};
    sceIoWrite(cache_write_fd_, &placeholder, sizeof(placeholder));
  }
  return true;
}

bool TitleBackground::append(const raster::Target& target) {
  pack565(target, packed_);
  vita2d_texture* texture = upload_packed(packed_.data(), target.width, target.height);
  if (texture == nullptr) {
    error_ = "title background ran out of texture memory";
    return false;
  }
  frames_.push_back(texture);

  if (cache_write_fd_ >= 0 && sceIoWrite(cache_write_fd_, packed_.data(), packed_.size()) !=
                                  static_cast<int>(packed_.size())) {
    // Out of space on the card. This boot is unaffected; it just will not be
    // able to skip the rasterising next time.
    finish_cache(false);
  }
  return true;
}

bool TitleBackground::step() {
  if (cache_read_fd_ >= 0) {
    return read_cached_frames(kCacheFramesPerStep);
  }
  if (source_ == nullptr) {
    return false;
  }

  Source& source = *source_;
  try {
    if (!source.in_sway) {
      source.target.reset(width_, height_);
      const Mat4 view_projection =
          raster::title_view_projection(source.dsk, source.frame, width_, height_);
      render_frame(source.target, source.opening, source.logo, view_projection, source.frame,
                   source.frame);
      if (!append(source.target)) {
        // A partial descent still plays, so what exists is kept.
        finish_cache(false);
        delete source_;
        source_ = nullptr;
        return false;
      }

      source.frame += frame_stride_;
      if (source.frame > kLastCinematicFrame) {
        // Once the camera lands, the flowers are the only thing still moving,
        // so one whole sway cycle is rendered at the settled camera and
        // repeated. The cycle is picked up where the descent left it, which
        // keeps the seam between the two invisible.
        loop_start_ = frames_.size();
        source.frame = 0.0f;
        source.in_sway = true;
      }
      return true;
    }

    if (source.frame >= source.sway_period) {
      finish_cache(true);
      delete source_;
      source_ = nullptr;
      return false;
    }

    source.target.reset(width_, height_);
    const Mat4 settled =
        raster::title_view_projection(source.dsk, kLastCinematicFrame, width_, height_);
    render_frame(source.target, source.opening, source.logo, settled, kLastCinematicFrame,
                 kLastCinematicFrame + source.frame);
    if (!append(source.target)) {
      finish_cache(false);
      delete source_;
      source_ = nullptr;
      return false;
    }
    source.frame += frame_stride_;
    return true;
  } catch (const std::exception& exception) {
    error_ = exception.what();
    finish_cache(false);
    delete source_;
    source_ = nullptr;
    return false;
  }
}

float TitleBackground::progress() const {
  if (source_ == nullptr && cache_read_fd_ < 0) {
    return 1.0f;
  }
  if (expected_frames_ == 0) {
    return 0.0f;
  }
  const float value = static_cast<float>(frames_.size()) / static_cast<float>(expected_frames_);
  return value > 1.0f ? 1.0f : value;
}

void TitleBackground::release() {
  for (size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i] != nullptr) {
      vita2d_free_texture(frames_[i]);
    }
  }
  frames_.clear();
  finish_cache(false);
  if (cache_read_fd_ >= 0) {
    sceIoClose(cache_read_fd_);
    cache_read_fd_ = -1;
  }
  delete source_;
  source_ = nullptr;
}

void TitleBackground::draw(float cinematic_frame) const {
  if (frames_.empty()) {
    return;
  }
  int index = static_cast<int>(cinematic_frame / frame_stride_);
  if (index < 0) {
    index = 0;
  }
  // Only repeat once the sway cycle is actually present; while the strip is
  // still being built there is nothing past the descent to loop over.
  const int loop_start = static_cast<int>(loop_start_);
  const int loop_length = static_cast<int>(frames_.size()) - loop_start;
  if (loop_start > 0 && index >= loop_start && loop_length > 0) {
    index = loop_start + (index - loop_start) % loop_length;
  }
  if (index >= static_cast<int>(frames_.size())) {
    index = static_cast<int>(frames_.size()) - 1;
  }
  vita2d_draw_texture_scale(frames_[static_cast<size_t>(index)], 0.0f, 0.0f,
                            static_cast<float>(kScreenWidth) / static_cast<float>(width_),
                            static_cast<float>(kScreenHeight) / static_cast<float>(height_));
}

}  // namespace render
