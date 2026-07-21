#ifndef __VSURFACE_PRIVATE_
#define __VSURFACE_PRIVATE_

// ***********************************************************************
// 
// PRIVATE, INTERNAL Header used by other SGP Internal modules
//
// Allows direct access to underlying Direct Draw Implementation
//
// ***********************************************************************

// Returns the raw 16bpp pixel buffer for a surface (WinFont builds a DIBSection over it).
BYTE* GetVideoSurfaceBuffer( HVSURFACE hVSurface );

#endif
