//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2012-2013, Image Engine Design Inc. All rights reserved.
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

#include "OP/OP_NodeInfoParms.h"
#include "PRM/PRM_ChoiceList.h"
#include "PRM/PRM_Default.h"
#include "UT/UT_StringMMPattern.h"

#include "IECore/CoordinateSystem.h"
#include "IECore/Group.h"
#include "IECore/TransformOp.h"
#include "IECore/VisibleRenderable.h"

#include "IECoreHoudini/GU_CortexPrimitive.h"
#include "IECoreHoudini/SOP_SceneCacheSource.h"
#include "IECoreHoudini/ToHoudiniAttribConverter.h"
#include "IECoreHoudini/ToHoudiniCortexObjectConverter.h"
#include "IECoreHoudini/ToHoudiniGeometryConverter.h"
#include "IECoreHoudini/ToHoudiniStringAttribConverter.h"

using namespace IECore;
using namespace IECoreHoudini;

static InternedString pName( "P" );

const char *SOP_SceneCacheSource::typeName = "ieSceneCacheSource";

PRM_Name SOP_SceneCacheSource::pObjectOnly( "objectOnly", "Object Only" );

OP_TemplatePair *SOP_SceneCacheSource::buildParameters()
{
	static PRM_Template *thisTemplate = 0;
	if ( !thisTemplate )
	{
		thisTemplate = new PRM_Template[2];
		
		thisTemplate[0] = PRM_Template(
			PRM_TOGGLE, 1, &pObjectOnly, 0, 0, 0, &sceneParmChangedCallback, 0, 0,
			"Determines whether this SOP cooks the current object only, or traverses down through the hierarchy."
		);
	}
	
	static OP_TemplatePair *templatePair = 0;
	if ( !templatePair )
	{
		OP_TemplatePair *firstTemplatePair = new OP_TemplatePair( thisTemplate );
		templatePair = new OP_TemplatePair( SceneCacheNode<SOP_Node>::parameters, firstTemplatePair );
	}
	
	return templatePair;
}

SOP_SceneCacheSource::SOP_SceneCacheSource( OP_Network *net, const char *name, OP_Operator *op ) : SceneCacheNode<SOP_Node>( net, name, op )
{
}

SOP_SceneCacheSource::~SOP_SceneCacheSource()
{
}

OP_Node *SOP_SceneCacheSource::create( OP_Network *net, const char *name, OP_Operator *op )
{
	return new SOP_SceneCacheSource( net, name, op );
}

bool SOP_SceneCacheSource::getObjectOnly() const
{
	return evalInt( pObjectOnly.getToken(), 0, 0 );
}

void SOP_SceneCacheSource::setObjectOnly( bool objectOnly )
{
	setInt( pObjectOnly.getToken(), 0, 0, objectOnly );
	sceneChanged();
}

void SOP_SceneCacheSource::sceneChanged()
{
	SceneCacheNode<SOP_Node>::sceneChanged();
	
	std::string file;
	if ( !ensureFile( file ) )
	{
		m_static = boost::indeterminate;
		return;
	}
	
	m_static = false;
	
	ConstSceneInterfacePtr scene = this->scene( file, getPath() );
	const SampledSceneInterface *sampledScene = IECore::runTimeCast<const SampledSceneInterface>( scene );
	if ( sampledScene )
	{
		bool objectOnly = this->evalInt( pObjectOnly.getToken(), 0, 0 );
		m_static = ( objectOnly && sampledScene->hasObject() ) ? ( sampledScene->numObjectSamples() < 2 ) : ( sampledScene->numBoundSamples() < 2 );
	}
	
	flags().setTimeDep( bool( !m_static ) );
}

OP_ERROR SOP_SceneCacheSource::cookMySop( OP_Context &context )
{
	// make sure the state is valid
	if ( boost::indeterminate( m_static ) )
	{
		sceneChanged();
	}
	
	flags().setTimeDep( bool( !m_static ) );
	
	std::string file;
	if ( !ensureFile( file ) )
	{
		addError( SOP_ATTRIBUTE_INVALID, ( file + " is not a valid .scc" ).c_str() );
		gdp->clearAndDestroy();
		return error();
	}
	
	std::string path = getPath();
	Space space = getSpace();
	GeometryType geometryType = (GeometryType)this->evalInt( pGeometryType.getToken(), 0, 0 );
	
	UT_String tagFilterStr;
	getTagFilter( tagFilterStr );
	UT_StringMMPattern tagFilter;
	tagFilter.compile( tagFilterStr );
	
	UT_String shapeFilterStr;
	getShapeFilter( shapeFilterStr );
	UT_StringMMPattern shapeFilter;
	shapeFilter.compile( shapeFilterStr );
	
	UT_String p( "P" );
	UT_String attributeFilter;
	getAttributeFilter( attributeFilter );
	if ( !p.match( attributeFilter ) )
	{
		attributeFilter += " P";
	}
	
	ConstSceneInterfacePtr scene = this->scene( file, path );
	if ( !scene )
	{
		addError( SOP_ATTRIBUTE_INVALID, ( path + " is not a valid location in " + file ).c_str() );
		gdp->clearAndDestroy();
		return error();
	}
	
	MurmurHash hash;
	hash.append( file );
	hash.append( path );
	hash.append( space );
	hash.append( tagFilterStr );
	hash.append( shapeFilterStr );
	hash.append( attributeFilter );
	hash.append( geometryType );
	hash.append( getObjectOnly() );
	
	if ( !m_loaded || m_hash != hash )
	{
		gdp->clearAndDestroy();
	}
	
	Imath::M44d transform = ( space == World ) ? worldTransform( file, path, context.getTime() ) : Imath::M44d();
	
	SceneInterface::Path rootPath;
	scene->path( rootPath );
	
	UT_Interrupt *progress = UTgetInterrupt();
	if ( !progress->opStart( ( "Cooking objects for " + getPath() ).c_str() ) )
	{
		addError( SOP_ATTRIBUTE_INVALID, "Cooking interrupted before it started" );
		gdp->clearAndDestroy();
		return error();
	}
	
	loadObjects( scene, transform, context.getTime(), space, tagFilter, shapeFilter, attributeFilter.toStdString(), geometryType, rootPath.size() );
	
	if ( progress->opInterrupt( 100 ) )
	{
		addError( SOP_ATTRIBUTE_INVALID, "Cooking interrupted" );
		gdp->clearAndDestroy();		
		m_loaded = false;
		m_hash = MurmurHash();
	}
	else
	{
		m_loaded = true;
		m_hash = hash;
	}
	
	progress->opEnd();
	
	return error();
}

void SOP_SceneCacheSource::loadObjects( const IECore::SceneInterface *scene, Imath::M44d transform, double time, Space space, const UT_StringMMPattern &tagFilter, const UT_StringMMPattern &shapeFilter, const std::string &attributeFilter, GeometryType geometryType, size_t rootSize )
{
	UT_Interrupt *progress = UTgetInterrupt();
	progress->setLongOpText( ( "Loading " + scene->name().string() ).c_str() );
	if ( progress->opInterrupt() )
	{
		return;
	}
	
	if ( scene->hasObject() && UT_String( scene->name() ).multiMatch( shapeFilter ) && tagged( scene, tagFilter ) )
	{
		ConstObjectPtr object = scene->readObject( time );
		std::string name = relativePath( scene, rootSize );
		
		bool hasAnimatedTopology = scene->hasAttribute( SceneCache::animatedObjectTopologyAttribute );
		bool hasAnimatedPrimVars = scene->hasAttribute( SceneCache::animatedObjectPrimVarsAttribute );
		std::vector<InternedString> animatedPrimVars;
		if ( hasAnimatedPrimVars )
		{
			const ConstObjectPtr animatedPrimVarObj = scene->readAttribute( SceneCache::animatedObjectPrimVarsAttribute, 0 );
			const InternedStringVectorData *animatedPrimVarData = IECore::runTimeCast<const InternedStringVectorData>( animatedPrimVarObj );
			if ( animatedPrimVarData )
			{
				const std::vector<InternedString> &values = animatedPrimVarData->readable();
				animatedPrimVars.resize( values.size() );
				std::copy( values.begin(), values.end(), animatedPrimVars.begin() );
			}
		}
		
		Imath::M44d currentTransform;
		if ( space == Local )
		{
			currentTransform = scene->readTransformAsMatrix( time );
		}
		else if ( space != Object )
		{
			currentTransform = transform;
		}
		
		// transform the object unless its an identity
		if ( currentTransform != Imath::M44d() )
		{
			object = transformObject( object, currentTransform, hasAnimatedTopology, hasAnimatedPrimVars, animatedPrimVars );
		}
		
		// convert the object to Houdini
		if ( !convertObject( object, name, attributeFilter, geometryType, hasAnimatedTopology, hasAnimatedPrimVars, animatedPrimVars ) )
		{
			std::string fullName;
			SceneInterface::Path path;
			scene->path( path );
			SceneInterface::pathToString( path, fullName );
			addWarning( SOP_MESSAGE, ( "Could not convert " + fullName + " to houdini" ).c_str() );
		}
	}
	
	if ( evalInt( pObjectOnly.getToken(), 0, 0 ) )
	{
		return;
	}
	
	SceneInterface::NameList children;
	scene->childNames( children );
	for ( SceneInterface::NameList::const_iterator it=children.begin(); it != children.end(); ++it )
	{
		ConstSceneInterfacePtr child = scene->child( *it );
		if ( tagged( child, tagFilter ) )
		{
			loadObjects( child, child->readTransformAsMatrix( time ) * transform, time, space, tagFilter, shapeFilter, attributeFilter, geometryType, rootSize );
		}
	}
}

ConstObjectPtr SOP_SceneCacheSource::transformObject( const IECore::Object *object, const Imath::M44d &transform, bool &hasAnimatedTopology, bool &hasAnimatedPrimVars, std::vector<InternedString> &animatedPrimVars )
{
	if ( const Primitive *primitive = IECore::runTimeCast<const Primitive>( object ) )
	{
		TransformOpPtr transformer = new TransformOp();
		transformer->inputParameter()->setValue( const_cast<Primitive*>( primitive ) ); // safe because we set the copy parameter
		transformer->copyParameter()->setTypedValue( true );
		transformer->matrixParameter()->setValue( new M44dData( transform ) );
		ObjectPtr result = transformer->operate();
		
		std::vector<std::string> &primVars = transformer->primVarsParameter()->getTypedValue();
		for ( std::vector<std::string>::iterator it = primVars.begin(); it != primVars.end(); ++it )
		{
			if ( std::find( animatedPrimVars.begin(), animatedPrimVars.end(), *it ) == animatedPrimVars.end() )
			{
				animatedPrimVars.push_back( *it );
				hasAnimatedPrimVars = true;
			}
		}
		
		return result;
	}
	else if ( const Group *group = IECore::runTimeCast<const Group>( object ) )
	{
		GroupPtr result = group->copy();
		MatrixTransformPtr matTransform = matrixTransform( transform );
		matTransform->matrix *= group->getTransform()->transform();
		result->setTransform( matTransform );
		return result;
	}
	else if ( const CoordinateSystem *coord = IECore::runTimeCast<const CoordinateSystem>( object ) )
	{
		CoordinateSystemPtr result = coord->copy();
		MatrixTransformPtr matTransform = matrixTransform( transform );
		matTransform->matrix *= coord->getTransform()->transform();
		result->setTransform( matTransform );
		return result;
	}
	
	return object;
}

bool SOP_SceneCacheSource::convertObject( const IECore::Object *object, const std::string &name, const std::string &attributeFilter, GeometryType geometryType, bool hasAnimatedTopology, bool hasAnimatedPrimVars, const std::vector<InternedString> &animatedPrimVars )
{
	ToHoudiniGeometryConverterPtr converter = 0;
	if ( geometryType == Cortex )
	{
		converter = new ToHoudiniCortexObjectConverter( object );
	}
	else
	{
		const VisibleRenderable *renderable = IECore::runTimeCast<const VisibleRenderable>( object );
		if ( !renderable )
		{
			return false;
		}
		
		converter = ToHoudiniGeometryConverter::create( renderable );
	}
	
	if ( !converter )
	{
		return false;
	}
	
	// attempt to optimize the conversion by re-using animated primitive variables
	const Primitive *primitive = IECore::runTimeCast<const Primitive>( object );
	GA_ROAttributeRef nameAttrRef = gdp->findStringTuple( GA_ATTRIB_PRIMITIVE, "name" );
	GA_Range primRange = gdp->getRangeByValue( nameAttrRef, name.c_str() );
	if ( primitive && !hasAnimatedTopology && hasAnimatedPrimVars && nameAttrRef.isValid() && !primRange.isEmpty() )
	{
		// this means constant topology and primitive variables, even though multiple samples were written
		if ( animatedPrimVars.empty() )
		{
			return true;
		}
		
		GA_Range pointRange( *gdp, primRange, GA_ATTRIB_POINT, GA_Range::primitiveref(), false );
		
		std::string animatedPrimVarStr = "";
		for ( std::vector<InternedString>::const_iterator it = animatedPrimVars.begin(); it != animatedPrimVars.end(); ++it )
		{
			animatedPrimVarStr += it->string() + " ";
		}
		
		converter->attributeFilterParameter()->setTypedValue( animatedPrimVarStr );
		converter->transferAttribs( gdp, pointRange, primRange );
		
		return true;
	}
	else
	{
		gdp->destroyPrimitives( primRange, true );
	}
	
	// fallback to full conversion
	converter->nameParameter()->setTypedValue( name );
	converter->attributeFilterParameter()->setTypedValue( attributeFilter );
	return converter->convert( myGdpHandle );
}

void SOP_SceneCacheSource::getNodeSpecificInfoText( OP_Context &context, OP_NodeInfoParms &parms )
{
	SceneCacheNode<SOP_Node>::getNodeSpecificInfoText( context, parms );
	
	// add type descriptions for the Cortex Objects
	GeometryType geometryType = (GeometryType)this->evalInt( pGeometryType.getToken(), 0, 0 );
	if ( geometryType == Cortex )
	{
		std::map<std::string, int> typeMap;
		const GU_Detail *geo = getCookedGeo( context );
		if ( !geo )
		{
			return;
		}
		
		const GA_PrimitiveList &primitives = geo->getPrimitiveList();
		for ( GA_Iterator it=geo->getPrimitiveRange().begin(); !it.atEnd(); ++it )
		{
			const GA_Primitive *prim = primitives.get( it.getOffset() );
			if ( prim->getTypeId() == GU_CortexPrimitive::typeId() )
			{
				const IECore::Object *object = ((GU_CortexPrimitive *)prim)->getObject();
				if ( object )
				{
					typeMap[object->typeName()] += 1;
				}
			}
		}
		
		if ( typeMap.empty() )
		{
			return;
		}
		
		parms.append( "Cortex Object Details:\n" );
		for ( std::map<std::string, int>::iterator it = typeMap.begin(); it != typeMap.end(); ++it )
		{
			parms.append( ( boost::format( "  %d " + it->first + "s\n" ) % it->second ).str().c_str() );
		}
		
		return;
	}
	
	// add conversion details for Houdini geo
	UT_String p( "P" );
	UT_String filter;
	evalString( filter, pAttributeFilter.getToken(), 0, 0 );
	if ( !p.match( filter ) )
	{
		filter += " P";
	}
	UT_StringMMPattern attributeFilter;
	attributeFilter.compile( filter );
	
	/// \todo: this text could come from a static method on a class that manages these name relations (once that exists)
	parms.append( "Converting standard Cortex PrimitiveVariables:\n" );
	if ( UT_String( "s" ).multiMatch( attributeFilter ) && UT_String( "t" ).multiMatch( attributeFilter ) )
	{
		parms.append( "  s,t -> uv\n" );
	}
	
	if ( UT_String( "Cs" ).multiMatch( attributeFilter ) )
	{
		parms.append( "  Cs -> Cd\n" );
	}
	
	if ( UT_String( "Pref" ).multiMatch( attributeFilter ) )
	{
		parms.append( "  Pref -> rest\n" );
	}
	
	if ( UT_String( "width" ).multiMatch( attributeFilter ) )
	{
		parms.append( "  width -> pscale\n" );
	}
	
	if ( UT_String( "Os" ).multiMatch( attributeFilter ) )
	{
		parms.append( "  Os -> Alpha\n" );
	}
}

MatrixTransformPtr SOP_SceneCacheSource::matrixTransform( Imath::M44d t )
{
	return new MatrixTransform(
		Imath::M44f(
			t[0][0], t[0][1], t[0][2], t[0][3],
			t[1][0], t[1][1], t[1][2], t[1][3],
			t[2][0], t[2][1], t[2][2], t[2][3],
			t[3][0], t[3][1], t[3][2], t[3][3]
		)
	);
}

std::string SOP_SceneCacheSource::relativePath( const IECore::SceneInterface *scene, size_t rootSize )
{
	SceneInterface::Path path, relative;
	scene->path( path );
	
	SceneInterface::Path::iterator start = path.begin() + rootSize;
	if ( start != path.end() )
	{
		relative.resize( path.end() - start );
		std::copy( start, path.end(), relative.begin() );
	}
	
	std::string result;
	SceneInterface::pathToString( relative, result );
	
	return result;
}
