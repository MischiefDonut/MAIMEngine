/*
** corona.zs
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2006-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

/******************************************************************************
 * 
 * Light coronas! (like in a certain un-real game...)
 * 
 * HOW TO USE:
 * 
 * Make a new actor inheriting from Corona, then create a Spawn state. Place
 * the actor in the map like you normally would any other decoration. The game
 * will draw the Spawn state's sprite in screen space.
 * 
 * The corona will be drawn with the actor's render style and alpha. It
 * defaults to the "Add" RenderStyle, but can be changed to any render style.
 * 
 * The actor's scale can also be used to influence the size of the corona
 * on screen.
 * 
 * The RenderRadius property can be used to set the maximum distance a corona
 * will be visible before it completely fades out. This also helps improve
 * performance - any coronas beyond the RenderRadius will not be processed
 * by the engine.
 * 
 * Currently, solid geometry and other solid actors will occlude the coronas.
 * Textures with transparent pixels do not block the corona. Lastly, corona
 * won't be reflected in mirrors.
 * 
 *****************************************************************************/

class Corona : Actor abstract
{
	Default
	{
		RenderStyle "Add";
		RenderRadius 1024.0;
		+BRIGHT
		+NOINTERACTION
		+NOGRAVITY
		+FORCEXYBILLBOARD
	}
}
