/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * A libGL between apitrace's tracer and gl4es (TRACE_LIBGL=this): it answers
 * the client-array queries apitrace makes to rebuild user-memory arrays.
 * gl4es is a GLES2 wrapper: it keeps the conventional vertex array in generic
 * attribute 0 (so apitrace saw attribute 0 enabled and faked a
 * glVertexAttribPointer(0) the replay can't use) and leaves GL_*_ARRAY_TYPE
 * and friends unanswered (so the faked calls had type 0).  Here those come
 * from the calls the game made.  Everything else is gl4es' (DT_NEEDED).
 *
 * Built by mctrace/build.sh; libgl4es-real.so.1 links to /usr/lib/gl4es/libGL.so.1
 * in a directory on LD_LIBRARY_PATH.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef void GLvoid;

#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_INDEX_ARRAY 0x8077
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_EDGE_FLAG_ARRAY 0x8079
#define GL_FOG_COORD_ARRAY 0x8457
#define GL_SECONDARY_COLOR_ARRAY 0x845E
#define GL_VERTEX_ARRAY_SIZE 0x807A
#define GL_VERTEX_ARRAY_TYPE 0x807B
#define GL_VERTEX_ARRAY_STRIDE 0x807C
#define GL_NORMAL_ARRAY_TYPE 0x807E
#define GL_NORMAL_ARRAY_STRIDE 0x807F
#define GL_COLOR_ARRAY_SIZE 0x8081
#define GL_COLOR_ARRAY_TYPE 0x8082
#define GL_COLOR_ARRAY_STRIDE 0x8083
#define GL_TEXTURE_COORD_ARRAY_SIZE 0x8088
#define GL_TEXTURE_COORD_ARRAY_TYPE 0x8089
#define GL_TEXTURE_COORD_ARRAY_STRIDE 0x808A
#define GL_VERTEX_ARRAY_POINTER 0x808E
#define GL_NORMAL_ARRAY_POINTER 0x808F
#define GL_COLOR_ARRAY_POINTER 0x8090
#define GL_TEXTURE_COORD_ARRAY_POINTER 0x8092
#define GL_ARRAY_BUFFER 0x8892
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_VERTEX_ARRAY_BUFFER_BINDING 0x8896
#define GL_NORMAL_ARRAY_BUFFER_BINDING 0x8897
#define GL_COLOR_ARRAY_BUFFER_BINDING 0x8898
#define GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING 0x889A
#define GL_CLIENT_ACTIVE_TEXTURE 0x84E1
#define GL_TEXTURE0 0x84C0
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING 0x889F

#define UNITS 8
#define ATTRIBS 16

struct array {
   GLint size, type, stride, buffer;
   const void *pointer;
   GLboolean enabled;
};

/* vertex, normal, colour, then a texture coordinate array per unit */
static struct array vertex = { 4, 0x1406, 0 }, normal = { 3, 0x1406, 0 }, color = { 4, 0x1406, 0 };
static struct array texcoord[UNITS] = { [0 ... UNITS - 1] = { 4, 0x1406, 0 } };
static GLuint client_unit, array_buffer;
static GLboolean attrib_enabled[ATTRIBS];

/* gl4es, the DT_NEEDED libgl4es-real.so.1 (a link to its libGL.so.1) */
static void *
gl4es(void)
{
   static void *h;
   if (!h)
      h = dlopen("libgl4es-real.so.1", RTLD_NOW | RTLD_NOLOAD);
   return h;
}

#define REAL(ret, name, args) \
   static ret (*real_##name) args; \
   if (!real_##name) real_##name = (ret (*) args)dlsym(gl4es(), #name)

static struct array *
array_of(GLenum cap)
{
   switch (cap) {
   case GL_VERTEX_ARRAY: return &vertex;
   case GL_NORMAL_ARRAY: return &normal;
   case GL_COLOR_ARRAY: return &color;
   case GL_TEXTURE_COORD_ARRAY: return &texcoord[client_unit];
   default: return NULL;
   }
}

static void
set(struct array *a, GLint size, GLenum type, GLsizei stride, const void *p)
{
   a->size = size, a->type = type, a->stride = stride, a->pointer = p, a->buffer = array_buffer;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
   REAL(void, glVertexPointer, (GLint, GLenum, GLsizei, const void *));
   set(&vertex, size, type, stride, p);
   real_glVertexPointer(size, type, stride, p);
}

void glNormalPointer(GLenum type, GLsizei stride, const void *p)
{
   REAL(void, glNormalPointer, (GLenum, GLsizei, const void *));
   set(&normal, 3, type, stride, p);
   real_glNormalPointer(type, stride, p);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
   REAL(void, glColorPointer, (GLint, GLenum, GLsizei, const void *));
   set(&color, size, type, stride, p);
   real_glColorPointer(size, type, stride, p);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p)
{
   REAL(void, glTexCoordPointer, (GLint, GLenum, GLsizei, const void *));
   set(&texcoord[client_unit], size, type, stride, p);
   real_glTexCoordPointer(size, type, stride, p);
}

static void
client_active(GLenum unit)
{
   if (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + UNITS)
      client_unit = unit - GL_TEXTURE0;
}

void glClientActiveTexture(GLenum unit)
{
   REAL(void, glClientActiveTexture, (GLenum));
   client_active(unit);
   real_glClientActiveTexture(unit);
}

void glClientActiveTextureARB(GLenum unit)
{
   REAL(void, glClientActiveTextureARB, (GLenum));
   client_active(unit);
   real_glClientActiveTextureARB(unit);
}

void glEnableClientState(GLenum cap)
{
   REAL(void, glEnableClientState, (GLenum));
   struct array *a = array_of(cap);
   if (a)
      a->enabled = 1;
   real_glEnableClientState(cap);
}

void glDisableClientState(GLenum cap)
{
   REAL(void, glDisableClientState, (GLenum));
   struct array *a = array_of(cap);
   if (a)
      a->enabled = 0;
   real_glDisableClientState(cap);
}

void glBindBuffer(GLenum target, GLuint buffer)
{
   REAL(void, glBindBuffer, (GLenum, GLuint));
   if (target == GL_ARRAY_BUFFER)
      array_buffer = buffer;
   real_glBindBuffer(target, buffer);
}

void glBindBufferARB(GLenum target, GLuint buffer)
{
   REAL(void, glBindBufferARB, (GLenum, GLuint));
   if (target == GL_ARRAY_BUFFER)
      array_buffer = buffer;
   real_glBindBufferARB(target, buffer);
}

void glEnableVertexAttribArray(GLuint i)
{
   REAL(void, glEnableVertexAttribArray, (GLuint));
   if (i < ATTRIBS)
      attrib_enabled[i] = 1;
   real_glEnableVertexAttribArray(i);
}

void glDisableVertexAttribArray(GLuint i)
{
   REAL(void, glDisableVertexAttribArray, (GLuint));
   if (i < ATTRIBS)
      attrib_enabled[i] = 0;
   real_glDisableVertexAttribArray(i);
}

GLboolean glIsEnabled(GLenum cap)
{
   REAL(GLboolean, glIsEnabled, (GLenum));
   struct array *a = array_of(cap);
   if (a)
      return a->enabled;
   if (cap == GL_INDEX_ARRAY || cap == GL_EDGE_FLAG_ARRAY || cap == GL_FOG_COORD_ARRAY ||
       cap == GL_SECONDARY_COLOR_ARRAY)
      return 0;
   return real_glIsEnabled(cap);
}

static int
query(GLenum pname, GLint *v)
{
   switch (pname) {
   case GL_VERTEX_ARRAY_SIZE: *v = vertex.size; return 1;
   case GL_VERTEX_ARRAY_TYPE: *v = vertex.type; return 1;
   case GL_VERTEX_ARRAY_STRIDE: *v = vertex.stride; return 1;
   case GL_VERTEX_ARRAY_BUFFER_BINDING: *v = vertex.buffer; return 1;
   case GL_NORMAL_ARRAY_TYPE: *v = normal.type; return 1;
   case GL_NORMAL_ARRAY_STRIDE: *v = normal.stride; return 1;
   case GL_NORMAL_ARRAY_BUFFER_BINDING: *v = normal.buffer; return 1;
   case GL_COLOR_ARRAY_SIZE: *v = color.size; return 1;
   case GL_COLOR_ARRAY_TYPE: *v = color.type; return 1;
   case GL_COLOR_ARRAY_STRIDE: *v = color.stride; return 1;
   case GL_COLOR_ARRAY_BUFFER_BINDING: *v = color.buffer; return 1;
   case GL_TEXTURE_COORD_ARRAY_SIZE: *v = texcoord[client_unit].size; return 1;
   case GL_TEXTURE_COORD_ARRAY_TYPE: *v = texcoord[client_unit].type; return 1;
   case GL_TEXTURE_COORD_ARRAY_STRIDE: *v = texcoord[client_unit].stride; return 1;
   case GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING: *v = texcoord[client_unit].buffer; return 1;
   case GL_CLIENT_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + client_unit; return 1;
   case GL_ARRAY_BUFFER_BINDING: *v = array_buffer; return 1;
   default: return 0;
   }
}

void glGetIntegerv(GLenum pname, GLint *v)
{
   REAL(void, glGetIntegerv, (GLenum, GLint *));
   if (!query(pname, v))
      real_glGetIntegerv(pname, v);
}

void glGetPointerv(GLenum pname, void **p)
{
   REAL(void, glGetPointerv, (GLenum, void **));
   switch (pname) {
   case GL_VERTEX_ARRAY_POINTER: *p = (void *)vertex.pointer; return;
   case GL_NORMAL_ARRAY_POINTER: *p = (void *)normal.pointer; return;
   case GL_COLOR_ARRAY_POINTER: *p = (void *)color.pointer; return;
   case GL_TEXTURE_COORD_ARRAY_POINTER: *p = (void *)texcoord[client_unit].pointer; return;
   }
   real_glGetPointerv(pname, p);
}

/* the game uses no generic attributes; gl4es' own (the conventional arrays
 * it keeps in them) are not the game's */
void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *v)
{
   REAL(void, glGetVertexAttribiv, (GLuint, GLenum, GLint *));
   if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED) {
      *v = index < ATTRIBS ? attrib_enabled[index] : 0;
      return;
   }
   real_glGetVertexAttribiv(index, pname, v);
}

/*
 * Reference frames for a replay on gl4es (glretrace with this as its
 * libGL.so.1): apitrace's snapshots and state dumps read pixels in formats
 * gl4es refuses, so here MCQ_SNAP="frame frame ..." frames are read as RGBA8
 * before their swap and written to MCQ_PREFIX<frame>.ppm (MCQ_W x MCQ_H, by
 * default 720x480).
 */
#include <stdio.h>
#include <stdlib.h>

#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_PACK_ALIGNMENT 0x0D05

static unsigned frame;

static void
snapshot(void)
{
   REAL(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
   REAL(void, glPixelStorei, (GLenum, GLint));
   const char *list = getenv("MCQ_SNAP"), *prefix = getenv("MCQ_PREFIX");
   char want[16], path[512];
   int w = getenv("MCQ_W") ? atoi(getenv("MCQ_W")) : 720, h = getenv("MCQ_H") ? atoi(getenv("MCQ_H")) : 480;

   frame++;
   if (!list || !prefix)
      return;
   snprintf(want, sizeof(want), " %u ", frame);
   char padded[4096];
   snprintf(padded, sizeof(padded), " %s ", list);
   if (!strstr(padded, want))
      return;
   unsigned char *px = malloc((size_t)w * h * 4);
   FILE *f;
   snprintf(path, sizeof(path), "%s%u.ppm", prefix, frame);
   if (!px || !(f = fopen(path, "wb"))) {
      free(px);
      return;
   }
   real_glPixelStorei(GL_PACK_ALIGNMENT, 1);
   real_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   fprintf(f, "P6\n%d %d\n255\n", w, h);
   for (int y = h - 1; y >= 0; y--)
      for (int x = 0; x < w; x++)
         fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
   fclose(f);
   free(px);
}

void glXSwapBuffers(void *dpy, unsigned long drawable)
{
   REAL(void, glXSwapBuffers, (void *, unsigned long));
   snapshot();
   real_glXSwapBuffers(dpy, drawable);
}

/* extension entry points the game gets by name come here too */
typedef void (*proc)(void);

static proc
ours(const char *name)
{
   static const struct { const char *name; proc fn; } t[] = {
      { "glVertexPointer", (proc)glVertexPointer }, { "glNormalPointer", (proc)glNormalPointer },
      { "glColorPointer", (proc)glColorPointer }, { "glTexCoordPointer", (proc)glTexCoordPointer },
      { "glClientActiveTexture", (proc)glClientActiveTexture },
      { "glClientActiveTextureARB", (proc)glClientActiveTextureARB },
      { "glEnableClientState", (proc)glEnableClientState }, { "glDisableClientState", (proc)glDisableClientState },
      { "glBindBuffer", (proc)glBindBuffer }, { "glBindBufferARB", (proc)glBindBufferARB },
      { "glEnableVertexAttribArray", (proc)glEnableVertexAttribArray },
      { "glDisableVertexAttribArray", (proc)glDisableVertexAttribArray },
      { "glIsEnabled", (proc)glIsEnabled }, { "glGetIntegerv", (proc)glGetIntegerv },
      { "glGetPointerv", (proc)glGetPointerv }, { "glGetVertexAttribiv", (proc)glGetVertexAttribiv },
   };
   for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
      if (!strcmp(name, t[i].name))
         return t[i].fn;
   return NULL;
}

proc glXGetProcAddressARB(const unsigned char *name)
{
   REAL(proc, glXGetProcAddressARB, (const unsigned char *));
   proc p = ours((const char *)name);
   return p ? p : real_glXGetProcAddressARB(name);
}

proc glXGetProcAddress(const unsigned char *name)
{
   REAL(proc, glXGetProcAddress, (const unsigned char *));
   proc p = ours((const char *)name);
   return p ? p : real_glXGetProcAddress(name);
}
