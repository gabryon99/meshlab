/****************************************************************************
* MeshLab                                                           o o     *
* A versatile mesh processing toolbox                             o     o   *
*                                                                _   O  _   *
* Copyright(C) 2005                                                \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
* This program is distributed in the hope that it will be useful,           *
* but WITHOUT ANY WARRANTY; without even the implied warranty of            *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
* GNU General Public License (http://www.gnu.org/licenses/gpl.txt)          *
* for more details.                                                         *
*                                                                           *
****************************************************************************/

#include <QtGui>

#include <math.h>
#include <stdlib.h>
#include <iostream>
#include "splatrenderer.h"
#include <QGLWidget>
#include <QTextStream>
#include <wrap/gl/trimesh.h>
#include <QGLFramebufferObject>

using namespace std;
using namespace vcg;

#define GL_TEST_ERR\
	{\
			GLenum eCode;\
			if((eCode=glGetError())!=GL_NO_ERROR)\
					std::cerr << "OpenGL error : " <<  gluErrorString(eCode) << " in " <<  __FILE__ << " : " << __LINE__ << std::endl;\
	}

SplatRendererPlugin::SplatRendererPlugin()
{
	mNormalTextureID = 0;
	mIsSupported = false;
	mRenderBuffer = 0;

	mFlags = DEFERRED_SHADING_BIT | DEPTH_CORRECTION_BIT | FLOAT_BUFFER_BIT;
	mCachedFlags = ~mFlags;
	// union of bits which controls the render buffer
	mRenderBufferMask = DEFERRED_SHADING_BIT | FLOAT_BUFFER_BIT;
}

void SplatRendererPlugin::initActionList()
{
	actionList << new QAction("Splatting", this);
}

QString SplatRendererPlugin::loadSource(const QString& func,const QString& filename)
{
	QString res;
	QFile f(":/SplatRenderer/shaders/" + filename);
	if (!f.open(QFile::ReadOnly))
	{
		std::cerr << "failed to load shader file " << filename.toAscii().data() << "\n";
		return res;
	}
	QTextStream stream(&f);
	res = stream.readAll();
	f.close();
	res = QString("#define __%1__ 1\n").arg(func)
			+ QString("#define %1 main\n").arg(func)
			+ res;
// 	std::cout << func.toAscii().data() << " loaded : \n" << res.toAscii().data() << "\n";
	return res;
}

void SplatRendererPlugin::configureShaders()
{
	QString defines = "";
	if (mFlags & DEFERRED_SHADING_BIT)
		defines += "#define EXPE_DEFERRED_SHADING\n";
	if (mFlags & DEPTH_CORRECTION_BIT)
		defines += "#define EXPE_DEPTH_CORRECTION\n";
	if (mFlags & OUTPUT_DEPTH_BIT)
		defines += "#define EXPE_OUTPUT_DEPTH\n";
	if (mFlags & BACKFACE_SHADING_BIT)
		defines += "#define EXPE_BACKFACE_SHADING\n";
	
	for (int k=0;k<3;++k)
	{
		QString vsrc = defines + mShaderSrcs[k*2+0];
		QString fsrc = defines + mShaderSrcs[k*2+1];
		mShaders[k].SetSources(mShaderSrcs[k*2+0]!="" ? vsrc.toAscii().data() : 0,
													 mShaderSrcs[k*2+1]!="" ? fsrc.toAscii().data() : 0);
		mShaders[k].prog.Link();
		std::string linkinfo = mShaders[k].prog.InfoLog();
		if (linkinfo.size()>0)
			std::cout << "Linked shader:\n" << linkinfo << "\n";
	}
}

void SplatRendererPlugin::Init(QAction *a, MeshModel &m, RenderMode &rm, QGLWidget *gla)
{
	mIsSupported = true;	
	gla->makeCurrent();
	// FIXME this should be done in meshlab !!! ??
	glewInit();

	// let's check the GPU capabilities
	mSupportedMask = DEPTH_CORRECTION_BIT | BACKFACE_SHADING_BIT;
	if (!QGLFramebufferObject::hasOpenGLFramebufferObjects ())
	{
		mIsSupported = false;
		return;
	}
	if (GLEW_ARB_texture_float)
		mSupportedMask |= FLOAT_BUFFER_BIT;
	if (GL_ATI_draw_buffers)
		mSupportedMask |= DEFERRED_SHADING_BIT;
	mFlags = mFlags & mSupportedMask;

	// load shader source
	mShaderSrcs[0] = loadSource("VisibilityVP","Raycasting.glsl");
	mShaderSrcs[1] = loadSource("VisibilityFP","Raycasting.glsl");
	mShaderSrcs[2] = loadSource("AttributeVP","Raycasting.glsl");
	mShaderSrcs[3] = loadSource("AttributeFP","Raycasting.glsl");
	mShaderSrcs[4] = "";
	mShaderSrcs[5] = loadSource("Finalization","Finalization.glsl");

	mCurrentPass = 2;
	mBindedPass = -1;
	GL_TEST_ERR
}

void SplatRendererPlugin::updateRenderBuffer()
{
	if ( (!mRenderBuffer)
		|| (mRenderBuffer->width()!=mCachedVP[2])
		|| (mRenderBuffer->height()!=mCachedVP[3])
		|| ( (mCachedFlags & mRenderBufferMask) != (mFlags & mRenderBufferMask) ))
	{
		delete mRenderBuffer;
		GLenum fmt = (mFlags&FLOAT_BUFFER_BIT) ? GL_RGBA16F_ARB : GL_RGBA;
		mRenderBuffer = new QGLFramebufferObject(mCachedVP[2], mCachedVP[3], QGLFramebufferObject::Depth, GL_TEXTURE_RECTANGLE_ARB, fmt);
		GL_TEST_ERR
		if (mFlags&DEFERRED_SHADING_BIT)
		{
			// add a second floating point render target for the normals
			if (mNormalTextureID==0)
				glGenTextures(1,&mNormalTextureID);
			glBindTexture(GL_TEXTURE_RECTANGLE_ARB, mNormalTextureID);
			glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, fmt, mCachedVP[2], mCachedVP[3], 0, GL_RGBA, GL_FLOAT, 0);
			glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			mRenderBuffer->bind();
			glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT, GL_TEXTURE_RECTANGLE_ARB, mNormalTextureID, 0);
			mRenderBuffer->release();
			GL_TEST_ERR
		}
	}
}

void SplatRendererPlugin::Render(QAction *a, MeshModel &m, RenderMode &rm, QGLWidget * /* gla */)
{
	GL_TEST_ERR
	mShaders[mCurrentPass].prog.Unbind();

	mCurrentPass = (mCurrentPass+1) % 3;

	if (mCurrentPass==0)
	{
		// this is the first pass of the frame, so let's update the shaders, buffers, etc...
		glGetIntegerv(GL_VIEWPORT, mCachedVP);
		glGetFloatv(GL_MODELVIEW_MATRIX, mCachedMV);
    glGetFloatv(GL_PROJECTION_MATRIX, mCachedProj);
		GL_TEST_ERR
		
		updateRenderBuffer(); GL_TEST_ERR
		if (mCachedFlags != mFlags)
			configureShaders();

		GL_TEST_ERR
		mCachedFlags = mFlags;
		
		mParams.update(mCachedMV, mCachedProj, mCachedVP);
		float s = m.glw.GetHintParamf(GLW::HNPPointSize);
		if (s>1)
			s = pow(s,0.3);
		mParams.radiusScale *= s;
		GL_TEST_ERR
	}
	
	if (mCurrentPass==2)
	{
		// this is the last pass: normalization by the sum of weights + deferred shading
		GL_TEST_ERR
		mRenderBuffer->release();
		if (mFlags&DEFERRED_SHADING_BIT)
			glDrawBuffer(GL_BACK);

		enablePass(mCurrentPass);

		// switch to normalized 2D rendering mode
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
		GL_TEST_ERR
		if (mFlags&DEFERRED_SHADING_BIT)
		{
			mShaders[2].prog.Uniform("unproj", mCachedProj[10], mCachedProj[14]);
			mShaders[2].prog.Uniform("NormalWeight",1.0f);
		}
		mShaders[2].prog.Uniform("viewport",float(mCachedVP[0]),float(mCachedVP[1]),float(mCachedVP[2]),float(mCachedVP[3]));
		mShaders[2].prog.Uniform("ColorWeight",0.0f);
		
    // bind the FBO's textures
		GL_TEST_ERR
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_RECTANGLE_ARB,mRenderBuffer->texture());
		if (mFlags&DEFERRED_SHADING_BIT)
		{
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_RECTANGLE_ARB,mNormalTextureID);
		}

		// draw a quad covering the whole screen
    vcg::Point3f viewVec(1./mCachedProj[0], 1./mCachedProj[5], -1);
    if (!(mFlags&OUTPUT_DEPTH_BIT))
		{
			glDisable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
		}
		GL_TEST_ERR
    glBegin(GL_QUADS);
			glColor3f(1, 0, 0);
			glTexCoord3f(viewVec.X(),viewVec.Y(),viewVec.Z());
			glMultiTexCoord2f(GL_TEXTURE1,1.,1.);
			glVertex3f(1,1,0);

			glColor3f(1, 1, 0);
			glTexCoord3f(-viewVec.X(),viewVec.Y(),viewVec.Z());
			glMultiTexCoord2f(GL_TEXTURE1,0.,1.);
			glVertex3f(-1,1,0);

			glColor3f(0, 1, 1);
			glTexCoord3f(-viewVec.X(),-viewVec.Y(),viewVec.Z());
			glMultiTexCoord2f(GL_TEXTURE1,0.,0.);
			glVertex3f(-1,-1,0);

			glColor3f(1, 0, 1);
			glTexCoord3f(viewVec.X(),-viewVec.Y(),viewVec.Z());
			glMultiTexCoord2f(GL_TEXTURE1,1.,0.);
			glVertex3f(1,-1,0);
    glEnd();
    GL_TEST_ERR
    if (!(mFlags&OUTPUT_DEPTH_BIT))
    {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }

		// restore matrices
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();
		GL_TEST_ERR
	}
	else
	{
		GL_TEST_ERR
		mParams.loadTo(mShaders[mCurrentPass].prog);
		if (mCurrentPass==0)
		{
			mRenderBuffer->bind();
			if (mFlags&DEFERRED_SHADING_BIT)
			{
				GLenum buf[2] = {GL_COLOR_ATTACHMENT0_EXT,GL_COLOR_ATTACHMENT1_EXT};
				glDrawBuffersARB(2, buf);
			}
			glViewport(mCachedVP[0],mCachedVP[1],mCachedVP[2],mCachedVP[3]);
			glClearColor(0,0,0,0);
			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
		}
		enablePass(mCurrentPass);
		GL_TEST_ERR
	}
}

void SplatRendererPlugin::Draw(QAction *a, MeshModel &m, RenderMode &rm, QGLWidget * gla)
{
	if (m.cm.vert.RadiusEnabled)
	{
		if (mCurrentPass==2)
			return;

		enablePass(mCurrentPass);
		drawSplats(m, rm);
	}
	else if (mCurrentPass==2)
	{
		MeshRenderInterface::Draw(a, m, rm, gla);
	}
}

void SplatRendererPlugin::enablePass(int n)
{
	if (mBindedPass!=n)
	{
		if (mBindedPass>=0)
			mShaders[mBindedPass].prog.Unbind();
		mShaders[n].prog.Bind();
		mBindedPass = n;

		// set GL states
		if (n==0)
		{
			glTexEnvf(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE);
			glEnable(GL_POINT_SPRITE_ARB);
			glDisable(GL_POINT_SMOOTH);
			glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
			
			glAlphaFunc(GL_LESS,1);
			glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glEnable(GL_ALPHA_TEST);
			glEnable(GL_DEPTH_TEST);
		}
		if (n==1)
		{
			glTexEnvf(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE);
			glEnable(GL_POINT_SPRITE_ARB);
			glDisable(GL_POINT_SMOOTH);
			glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
			
			glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE,GL_ONE);
			glDepthMask(GL_FALSE);
			glEnable(GL_BLEND);
			glEnable(GL_DEPTH_TEST);
			glDisable(GL_ALPHA_TEST);
		}
		if (n==2)
		{
			glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
			glDepthMask(GL_TRUE);
			glDisable(GL_LIGHTING);
			glDisable(GL_BLEND);
		}
	}
}

void SplatRendererPlugin::drawSplats(MeshModel &m, RenderMode &rm)
{
	if(m.cm.vn!=(int)m.cm.vert.size())
	{
		// manual rendering
		int cm = rm.colorMode;
		if( (cm == GLW::CMPerFace)  && (!tri::HasPerFaceColor(m.cm)) )
			cm=GLW::CMNone;
		glPushMatrix();
		glMultMatrix(m.cm.Tr);
		CMeshO::VertexIterator vi;
		glBegin(GL_POINTS);
			if(cm==GLW::CMPerMesh)
				glColor(m.cm.C());

			for(vi=m.cm.vert.begin();vi!=m.cm.vert.end();++vi)
				if(!(*vi).IsD())
				{
					glMultiTexCoord1f(GL_TEXTURE2, (*vi).cR());
					glNormal((*vi).cN());
					if (cm==GLW::CMPerVert) glColor((*vi).C());
					glVertex((*vi).P());
				}
		glEnd();
		glPopMatrix();
		return;
	}

	// bind the radius
	glClientActiveTexture(GL_TEXTURE2);
	glTexCoordPointer(
		1,
		GL_FLOAT,
		size_t(m.cm.vert[1].cR())-size_t(m.cm.vert[0].cR()),
		&m.cm.vert[0].cR()
	);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);

	// draw the vertices
	m.Render(vcg::GLW::DMPoints,rm.colorMode,rm.textureMode);

	glClientActiveTexture(GL_TEXTURE2);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
}

void SplatRendererPlugin::UniformParameters::update(float* mv, float* proj, GLint* vp)
{
	// extract the uniform scale
	float scale = vcg::Point3f(mv[0],mv[1],mv[2]).Norm();

	radiusScale = scale;
	preComputeRadius = - std::max(proj[0]*vp[2], proj[5]*vp[3]);
	depthOffset = 2.0;
	oneOverEwaRadius = 0.70710678118654;
	halfVp = Point2f(0.5*vp[2], 0.5*vp[3]);
	rayCastParameter1 = Point3f(2./(proj[0]*vp[2]), 2./(proj[5]*vp[3]), 0.0);
	rayCastParameter2 = Point3f(-1./proj[0], -1./proj[5], -1.0);
	depthParameterCast = Point2f(0.5*proj[14], 0.5-0.5*proj[10]);
}

void SplatRendererPlugin::UniformParameters::loadTo(Program& prg)
{
	prg.Bind();
	prg.Uniform("expeRadiusScale",radiusScale);
	prg.Uniform("expePreComputeRadius",preComputeRadius);
	prg.Uniform("expeDepthOffset",depthOffset);
	prg.Uniform("oneOverEwaRadius",oneOverEwaRadius);
	prg.Uniform("halfVp",halfVp);
	prg.Uniform("rayCastParameter1",rayCastParameter1);
	prg.Uniform("rayCastParameter2",rayCastParameter2);
	prg.Uniform("depthParameterCast",depthParameterCast);
}

Q_EXPORT_PLUGIN(SplatRendererPlugin)
