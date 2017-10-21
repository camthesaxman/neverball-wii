/*
 * Copyright (C) 2003-2011 Neverball authors
 *
 * NEVERBALL is  free software; you can redistribute  it and/or modify
 * it under the  terms of the GNU General  Public License as published
 * by the Free  Software Foundation; either version 2  of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT  ANY  WARRANTY;  without   even  the  implied  warranty  of
 * MERCHANTABILITY or  FITNESS FOR A PARTICULAR PURPOSE.   See the GNU
 * General Public License for more details.
 */

#ifndef GLEXT_H
#define GLEXT_H

/*---------------------------------------------------------------------------*/
/* Include the system OpenGL headers.                                        */

#include "wiigl.h"

/*---------------------------------------------------------------------------*/

int glext_check(const char *);
int glext_init(void);

/*---------------------------------------------------------------------------*/

/* Exercise extreme paranoia in defining a cross-platform OpenGL interface.  */
/* If we're compiling against OpenGL ES then we must assume native linkage   */
/* of the extensions we use. Otherwise, GetProc them regardless of whether   */
/* they need it or not.                                                      */

void glClipPlane4f_(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);

/*---------------------------------------------------------------------------*/

struct gl_info
{
    GLint max_texture_units;
    GLint max_texture_size;

    unsigned int texture_filter_anisotropic : 1;
    unsigned int shader_objects             : 1;
    unsigned int framebuffer_object         : 1;
};

extern struct gl_info gli;

/*---------------------------------------------------------------------------*/
#endif
