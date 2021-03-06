//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2013, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of Image Engine Design nor the names of any
//       other contributors to this software may be used to endorse or
//       promote products derived from this software without specific prior
//       written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

/// GR_Primitives are used in Houdini 12.5, but do not exist in earlier versions.
/// Check GR_Cortex.cpp for Cortex viewport rendering in Houdini 12.0.
#include "UT/UT_Version.h"
#if UT_MAJOR_VERSION_INT > 12 || UT_MINOR_VERSION_INT >= 5

#include "IECore/MeshPrimitive.h"
#include "IECore/SimpleTypedData.h"

#include "IECoreGL/IECoreGL.h"
#include "IECoreGL/Camera.h"
#include "IECoreGL/CurvesPrimitive.h"
#include "IECoreGL/PointsPrimitive.h"
#include "IECoreGL/Primitive.h"
#include "IECoreGL/Renderer.h"
#include "IECoreGL/Scene.h"
#include "IECoreGL/ShaderLoader.h"
#include "IECoreGL/ShaderStateComponent.h"
#include "IECoreGL/State.h"
#include "IECoreGL/TextureLoader.h"

// this needs to come after IECoreGL so gl and glew don't fight
#include "RE/RE_Render.h"

#include "IECoreHoudini/Convert.h"
#include "IECoreHoudini/GR_CortexPrimitive.h"
#include "IECoreHoudini/GU_CortexPrimitive.h"

using namespace IECoreHoudini;

GR_CortexPrimitive::GR_CortexPrimitive( const GR_RenderInfo *info, const char *cache_name, const GEO_Primitive *prim )
	: GR_Primitive( info, cache_name, GA_PrimCompat::TypeMask(0) )
{
	IECoreGL::init( true );
	
	if ( prim->getTypeDef().getId() == GU_CortexPrimitive::typeId().get() )
	{
		m_primId = prim->getNum();
	}
}

GR_CortexPrimitive::~GR_CortexPrimitive()
{
	m_renderable = 0;
}

GR_PrimAcceptResult GR_CortexPrimitive::acceptPrimitive( GT_PrimitiveType t, int geo_type, const GT_PrimitiveHandle &ph, const GEO_Primitive *prim )
{
	if ( geo_type == GU_CortexPrimitive::typeId().get() )
	{
		m_primId = prim->getNum();
		return GR_PROCESSED;
	}
	
	return GR_NOT_PROCESSED;
}

void GR_CortexPrimitive::resetPrimitives()
{
	m_primId = -1;
	m_renderable = 0;
}

void GR_CortexPrimitive::update( RE_Render *r, const GT_PrimitiveHandle &primh, const GR_UpdateParms &p )
{
	GA_Offset offset = p.geometry.primitiveOffset( m_primId );
	const GU_CortexPrimitive *prim = dynamic_cast<const GU_CortexPrimitive *>( p.geometry.getGEOPrimitive( offset ) );
	if ( !prim )
	{
		m_scene = 0;
		m_renderable = 0;
		return;
	}
	
	m_renderable = IECore::runTimeCast<const IECore::Renderable>( prim->getObject() );
	if ( !m_renderable )
	{
		m_scene = 0;
		return;
	}
	
	IECoreGL::RendererPtr renderer = new IECoreGL::Renderer();
	renderer->setOption( "gl:mode", new IECore::StringData( "deferred" ) );
	renderer->setOption( "gl:drawCoordinateSystems", new IECore::BoolData( true ) );
	renderer->worldBegin();
	
	if ( p.dopts.boundBox() )
	{
		const IECore::VisibleRenderable *visible = IECore::runTimeCast<const IECore::VisibleRenderable>( m_renderable );
		if ( visible )
		{
			IECore::MeshPrimitive::createBox( visible->bound() )->render( renderer );
		}
	}
	else
	{
		m_renderable->render( renderer );
	}
	
	renderer->worldEnd();
	
	m_scene = renderer->scene();
	m_scene->setCamera( 0 ); // houdini will be providing the camera
}

void GR_CortexPrimitive::render( RE_Render *r, GR_RenderMode render_mode, GR_RenderFlags flags, const GR_DisplayOption *opt, const RE_MaterialList *materials )
{
	if ( !m_scene )
	{
		return;
	}
	
	UT_Matrix4D transform;
	memcpy( transform.data(), r->getUniform( RE_UNIFORM_OBJECT_MATRIX )->getValue(), sizeof(double) * 16 );
	
	r->pushMatrix();
		
		r->multiplyMatrix( transform );
		m_scene->render( getState( render_mode, flags, opt ) );
	
	r->popMatrix();
}

IECoreGL::StatePtr GR_CortexPrimitive::g_lit = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_shaded = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_wire = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_wireLit = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_wireShaded = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_wireConstGhost = 0;
IECoreGL::StatePtr GR_CortexPrimitive::g_wireConstBG = 0;

IECoreGL::State *GR_CortexPrimitive::getState( GR_RenderMode mode, GR_RenderFlags flags, const GR_DisplayOption *opt )
{
	if ( !g_lit || !g_shaded || !g_wire || !g_wireLit || !g_wireShaded || !g_wireConstGhost || !g_wireConstBG )
	{
		g_shaded = new IECoreGL::State( true );
		g_shaded->add( new IECoreGL::PointsPrimitive::UseGLPoints( IECoreGL::ForAll ) );
		g_shaded->add( new IECoreGL::PointsPrimitive::GLPointWidth( 3. ) );
		g_shaded->add( new IECoreGL::CurvesPrimitive::UseGLLines( true ) );
		
		/// \todo: this doesn't seem to get the lights. maybe they aren't in the gl light list?
		g_lit = new IECoreGL::State( *g_shaded );
		g_lit->add(
			new IECoreGL::ShaderStateComponent(
				IECoreGL::ShaderLoader::defaultShaderLoader(),
				IECoreGL::TextureLoader::defaultTextureLoader(),
				IECoreGL::Shader::defaultVertexSource(),
				IECoreGL::Shader::defaultGeometrySource(),
				IECoreGL::Shader::lambertFragmentSource(),
				new IECore::CompoundObject()
			),
			/// \todo: by setting true here, we are forcing an override of all other
			/// ShaderStateComponents in the hierarhcy. Is this desirable in all cases?
			true
		);
		
		g_wireShaded = new IECoreGL::State( *g_shaded );
		g_wireShaded->add( new IECoreGL::Primitive::DrawWireframe( true ) );
		g_wireShaded->add( new IECoreGL::WireframeColorStateComponent( IECore::convert<Imath::Color4f>( opt->common().getColor( GR_WIREFRAME_COLOR ) ) ) );
		
		g_wire = new IECoreGL::State( *g_shaded );
		g_wire->add( new IECoreGL::Primitive::DrawSolid( false ) );
		g_wire->add( new IECoreGL::Primitive::DrawWireframe( true ) );
		g_wire->add( new IECoreGL::WireframeColorStateComponent( Imath::Color4f( 1 ) ) );
		
		g_wireLit = new IECoreGL::State( *g_lit );
		g_wireLit->add( new IECoreGL::Primitive::DrawWireframe( true ) );
		g_wireLit->add( new IECoreGL::WireframeColorStateComponent( Imath::Color4f( 0.5, 0.5, 0.5, 1 ) ) );
		
		g_wireConstBG = new IECoreGL::State( *g_wireShaded );
		g_wireConstBG->add( new IECoreGL::Color( IECore::convert<Imath::Color4f>( opt->common().getColor( GR_BACKGROUND_COLOR ) ) ) );
		g_wireConstBG->add(
			new IECoreGL::ShaderStateComponent(
				IECoreGL::ShaderLoader::defaultShaderLoader(),
				IECoreGL::TextureLoader::defaultTextureLoader(),
				IECoreGL::Shader::defaultVertexSource(),
				IECoreGL::Shader::defaultGeometrySource(),
				IECoreGL::Shader::constantFragmentSource(),
				new IECore::CompoundObject()
			),
			true
		);
		
		g_wireConstGhost = new IECoreGL::State( *g_wireConstBG );
		g_wireConstGhost->add( new IECoreGL::Color( IECore::convert<Imath::Color4f>( opt->common().getColor( GR_GHOST_FILL_COLOR ) ) ) );
	}
	
	switch ( mode )
	{
		case GR_RENDER_BEAUTY :
		case GR_RENDER_MATERIAL :
		case GR_RENDER_MATERIAL_WIREFRAME :
		{
			if ( flags & GR_RENDER_FLAG_WIRE_OVER )
			{
				if ( flags & GR_RENDER_FLAG_UNLIT )
				{
					return g_wireShaded;
				}
				
				return g_wireLit;
			}
			
			if ( flags & GR_RENDER_FLAG_UNLIT )
			{
				return g_shaded;
			}
			
			return g_lit;
		}
		case GR_RENDER_WIREFRAME :
		{
			return g_wire;
		}
		case GR_RENDER_HIDDEN_LINE :
		{
			return g_wireConstBG;
		}
		case GR_RENDER_GHOST_LINE :
		{
			return g_wireConstGhost;
		}
		default :
		{
			break;
		}
	}
	
	return g_shaded;
}

#endif // 12.5 or later
