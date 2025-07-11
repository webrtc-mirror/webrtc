/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/linux/glx_renderer.h"

#include <GL/glx.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>

#include <cstdlib>

#include "api/video/video_frame.h"
#include "rtc_base/checks.h"
#include "test/gl/gl_renderer.h"

namespace webrtc {
namespace test {

GlxRenderer::GlxRenderer(size_t width, size_t height)
    : width_(width), height_(height), display_(nullptr), context_(nullptr) {
  RTC_DCHECK_GT(width, 0);
  RTC_DCHECK_GT(height, 0);
}

GlxRenderer::~GlxRenderer() {
  Destroy();
}

bool GlxRenderer::Init(const char* window_title) {
  if ((display_ = XOpenDisplay(nullptr)) == nullptr) {
    Destroy();
    return false;
  }

  int screen = DefaultScreen(display_);

  XVisualInfo* vi;
  int attr_list[] = {
      GLX_DOUBLEBUFFER, GLX_RGBA, GLX_RED_SIZE,   4,  GLX_GREEN_SIZE, 4,
      GLX_BLUE_SIZE,    4,        GLX_DEPTH_SIZE, 16, None,
  };

  if ((vi = glXChooseVisual(display_, screen, attr_list)) == nullptr) {
    Destroy();
    return false;
  }

  context_ = glXCreateContext(display_, vi, nullptr, true);
  if (context_ == nullptr) {
    Destroy();
    return false;
  }

  XSetWindowAttributes window_attributes;
  window_attributes.colormap = XCreateColormap(
      display_, RootWindow(display_, vi->screen), vi->visual, AllocNone);
  window_attributes.border_pixel = 0;
  window_attributes.event_mask = StructureNotifyMask | ExposureMask;
  window_ = XCreateWindow(display_, RootWindow(display_, vi->screen), 0, 0,
                          width_, height_, 0, vi->depth, InputOutput,
                          vi->visual, CWBorderPixel | CWColormap | CWEventMask,
                          &window_attributes);
  XFree(vi);

  XSetStandardProperties(display_, window_, window_title, window_title, None,
                         nullptr, 0, nullptr);

  Atom wm_delete = XInternAtom(display_, "WM_DELETE_WINDOW", True);
  if (wm_delete != None) {
    XSetWMProtocols(display_, window_, &wm_delete, 1);
  }

  XMapRaised(display_, window_);

  if (!glXMakeCurrent(display_, window_, context_)) {
    Destroy();
    return false;
  }
  GlRenderer::Init();
  if (!glXMakeCurrent(display_, None, nullptr)) {
    Destroy();
    return false;
  }

  Resize(width_, height_);
  return true;
}

void GlxRenderer::Destroy() {
  if (context_ != nullptr) {
    glXMakeCurrent(display_, window_, context_);
    GlRenderer::Destroy();
    glXMakeCurrent(display_, None, nullptr);
    glXDestroyContext(display_, context_);
    context_ = nullptr;
  }

  if (display_ != nullptr) {
    XCloseDisplay(display_);
    display_ = nullptr;
  }
}

GlxRenderer* GlxRenderer::Create(const char* window_title,
                                 size_t width,
                                 size_t height) {
  GlxRenderer* glx_renderer = new GlxRenderer(width, height);
  if (!glx_renderer->Init(window_title)) {
    // TODO(pbos): Add GLX-failed warning here?
    delete glx_renderer;
    return nullptr;
  }
  return glx_renderer;
}

void GlxRenderer::Resize(size_t width, size_t height) {
  width_ = width;
  height_ = height;
  if (!glXMakeCurrent(display_, window_, context_)) {
    abort();
  }
  GlRenderer::ResizeViewport(width_, height_);
  if (!glXMakeCurrent(display_, None, nullptr)) {
    abort();
  }

  XSizeHints* size_hints = XAllocSizeHints();
  if (size_hints == nullptr) {
    abort();
  }
  size_hints->flags = PAspect;
  size_hints->min_aspect.x = size_hints->max_aspect.x = width_;
  size_hints->min_aspect.y = size_hints->max_aspect.y = height_;
  XSetWMNormalHints(display_, window_, size_hints);
  XFree(size_hints);

  XWindowChanges wc;
  wc.width = static_cast<int>(width);
  wc.height = static_cast<int>(height);
  XConfigureWindow(display_, window_, CWWidth | CWHeight, &wc);
}

void GlxRenderer::OnFrame(const VideoFrame& frame) {
  if (static_cast<size_t>(frame.width()) != width_ ||
      static_cast<size_t>(frame.height()) != height_) {
    Resize(static_cast<size_t>(frame.width()),
           static_cast<size_t>(frame.height()));
  }

  XEvent event;
  if (!glXMakeCurrent(display_, window_, context_)) {
    abort();
  }
  while (XPending(display_)) {
    XNextEvent(display_, &event);
    switch (event.type) {
      case ConfigureNotify:
        GlRenderer::ResizeViewport(event.xconfigure.width,
                                   event.xconfigure.height);
        break;
      default:
        break;
    }
  }

  GlRenderer::OnFrame(frame);
  glXSwapBuffers(display_, window_);

  if (!glXMakeCurrent(display_, None, nullptr)) {
    abort();
  }
}
}  // namespace test
}  // namespace webrtc
