/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

#ifndef _R3DSCROLLFOG_H_
#define _R3DSCROLLFOG_H_

#include <GL/glew.h>

namespace New3D {

	class R3DScrollFog
	{
	public:

		R3DScrollFog();
		~R3DScrollFog();

		void DrawScrollFog(float rbga[4], float attenuation, float ambient, float spotRGB[3], float spotEllipse[4]);

		// PC VR: while drawing a headset eye, the spotlight-on-fog ellipse is a place on the game's
		// screen (ellipse in game screen pixels x scale). eyeView: the eye's view matrix,
		// eyeTan: its left/right/up/down tangents, eyeSize: its framebuffer size, gameTan: the
		// game screen's left/right/top/bottom tangents. SetVR(nullptr, ...) turns it off.
		void SetVR(const float* eyeView, const float eyeTan[4], float eyeW, float eyeH, const float gameTan[4], float scaleX, float scaleY);

	private:

		void AllocResources();
		void DeallocResources();

		GLuint m_shaderProgram;
		GLuint m_vertexShader;
		GLuint m_fragmentShader;

		GLint m_locFogColour;
		GLint m_locFogAttenuation;
		GLint m_locFogAmbient;
		GLint m_locSpotFogColor;
		GLint m_locSpotEllipse;
		GLint m_locVRSpot, m_locVRInvRot, m_locVREyeTan, m_locVREyeSize, m_locVRGameTan, m_locVRScale;

		bool  m_vrOn = false;
		float m_vrInvRot[9] = {};
		float m_vrEyeTan[4] = {};
		float m_vrEyeSize[2] = {};
		float m_vrGameTan[4] = {};
		float m_vrScale[2] = {1.f, 1.f};

		GLuint m_vao;
	};

}

#endif