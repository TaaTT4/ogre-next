/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreStableHeaders.h"
// RenderSystem implementation
// Note that most of this class is abstract since
//  we cannot know how to implement the behaviour without
//  being aware of the 3D API. However there are a few
//  simple functions which can have a base implementation

#include "OgreRenderSystem.h"

#include "OgreRoot.h"
#include "OgreViewport.h"
#include "OgreException.h"
#include "OgreDepthBuffer.h"
#include "OgreIteratorWrappers.h"
#include "OgreLogManager.h"
#include "OgreMaterialManager.h"
#include "OgreHardwareOcclusionQuery.h"
#include "OgreHlmsPso.h"
#include "OgreTextureGpuManager.h"
#include "OgreWindow.h"
#include "Compositor/OgreCompositorManager2.h"
#include "Vao/OgreVaoManager.h"
#include "Vao/OgreVertexArrayObject.h"
#include "OgreProfiler.h"

#include "OgreLwString.h"

namespace Ogre {

    RenderSystem::ListenerList  RenderSystem::msSharedEventListeners;
    //-----------------------------------------------------------------------
    RenderSystem::RenderSystem()
        : mCurrentRenderPassDescriptor(0)
        , mMaxBoundViewports(16u)
        , mVaoManager(0)
        , mTextureGpuManager(0)
#if OGRE_DEBUG_MODE >= OGRE_DEBUG_HIGH
        , mDebugShaders(true)
#else
        , mDebugShaders(false)
#endif
        , mWBuffer(false)
        , mInvertVertexWinding(false)
        , mDisabledTexUnitsFrom(0)
        , mCurrentPassIterationCount(0)
        , mCurrentPassIterationNum(0)
        , mDerivedDepthBias(false)
        , mDerivedDepthBiasBase(0.0f)
        , mDerivedDepthBiasMultiplier(0.0f)
        , mDerivedDepthBiasSlopeScale(0.0f)
        , mUavRenderingDirty(false)
        , mUavStartingSlot( 1 )
        , mUavRenderingDescSet( 0 )
        , mGlobalInstanceVertexBufferVertexDeclaration(NULL)
        , mGlobalNumberOfInstances(1)
        , mVertexProgramBound(false)
        , mGeometryProgramBound(false)
        , mFragmentProgramBound(false)
        , mTessellationHullProgramBound(false)
        , mTessellationDomainProgramBound(false)
        , mComputeProgramBound(false)
        , mClipPlanesDirty(true)
        , mRealCapabilities(0)
        , mCurrentCapabilities(0)
        , mUseCustomCapabilities(false)
        , mNativeShadingLanguageVersion(0)
        , mTexProjRelative(false)
        , mTexProjRelativeOrigin(Vector3::ZERO)
        , mReverseDepth(true)
    {
        mEventNames.push_back("RenderSystemCapabilitiesCreated");
    }
    //-----------------------------------------------------------------------
    RenderSystem::~RenderSystem()
    {
        shutdown();
        OGRE_DELETE mRealCapabilities;
        mRealCapabilities = 0;
        // Current capabilities managed externally
        mCurrentCapabilities = 0;
    }
    //-----------------------------------------------------------------------
    Window *RenderSystem::_initialise( bool autoCreateWindow, const String& windowTitle )
    {
        // Have I been registered by call to Root::setRenderSystem?
        /** Don't do this anymore, just allow via Root
        RenderSystem* regPtr = Root::getSingleton().getRenderSystem();
        if (!regPtr || regPtr != this)
            // Register self - library user has come to me direct
            Root::getSingleton().setRenderSystem(this);
        */


        // Subclasses should take it from here
        // They should ALL call this superclass method from
        //   their own initialise() implementations.
        
        mVertexProgramBound = false;
        mGeometryProgramBound = false;
        mFragmentProgramBound = false;
        mTessellationHullProgramBound = false;
        mTessellationDomainProgramBound = false;
        mComputeProgramBound = false;

        return 0;
    }

    //---------------------------------------------------------------------------------------------
    void RenderSystem::useCustomRenderSystemCapabilities(RenderSystemCapabilities* capabilities)
    {
    if (mRealCapabilities != 0)
    {
      OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, 
          "Custom render capabilities must be set before the RenderSystem is initialised.",
          "RenderSystem::useCustomRenderSystemCapabilities");
    }

        mCurrentCapabilities = capabilities;
        mUseCustomCapabilities = true;
    }

    //---------------------------------------------------------------------------------------------
    bool RenderSystem::_createRenderWindows(const RenderWindowDescriptionList& renderWindowDescriptions, 
        WindowList &createdWindows)
    {
        unsigned int fullscreenWindowsCount = 0;

        // Grab some information and avoid duplicate render windows.
        for (unsigned int nWindow=0; nWindow < renderWindowDescriptions.size(); ++nWindow)
        {
            const RenderWindowDescription* curDesc = &renderWindowDescriptions[nWindow];

            // Count full screen windows.
            if (curDesc->useFullScreen)         
                fullscreenWindowsCount++;   

            bool renderWindowFound = false;

            for( unsigned int nSecWindow = nWindow + 1; nSecWindow < renderWindowDescriptions.size();
                 ++nSecWindow )
            {
                if( curDesc->name == renderWindowDescriptions[nSecWindow].name )
                {
                    renderWindowFound = true;
                    break;
                }
            }

            // Make sure we don't already have a render target of the 
            // same name as the one supplied
            if(renderWindowFound)
            {
                String msg;

                msg = "A render target of the same name '" + String(curDesc->name) + "' already "
                    "exists.  You cannot create a new window with this name.";
                OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR, msg, "RenderSystem::createRenderWindow" );
            }
        }
        
        // Case we have to create some full screen rendering windows.
        if (fullscreenWindowsCount > 0)
        {
            // Can not mix full screen and windowed rendering windows.
            if (fullscreenWindowsCount != renderWindowDescriptions.size())
            {
                OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, 
                    "Can not create mix of full screen and windowed rendering windows",
                    "RenderSystem::createRenderWindows");
            }                   
        }

        return true;
    }

    //---------------------------------------------------------------------------------------------
    void RenderSystem::destroyRenderWindow( Window *window )
    {
        WindowSet::iterator itor = mWindows.find( window );

        if( itor == mWindows.end() )
        {
            OGRE_EXCEPT( Exception::ERR_ITEM_NOT_FOUND,
                         "Window does not belong to us or is already deleted!",
                         "RenderSystem::destroyRenderWindow" );
        }
        mWindows.erase( window );
        OGRE_DELETE window;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setPipelineStateObject( const HlmsPso *pso )
    {
        assert( (!pso || pso->rsData) &&
                "The PipelineStateObject must have been created via "
                "RenderSystem::_hlmsPipelineStateObjectCreated!" );

        //Disable previous state
        mActiveVertexGpuProgramParameters.setNull();
        mActiveGeometryGpuProgramParameters.setNull();
        mActiveTessellationHullGpuProgramParameters.setNull();
        mActiveTessellationDomainGpuProgramParameters.setNull();
        mActiveFragmentGpuProgramParameters.setNull();
        mActiveComputeGpuProgramParameters.setNull();

        if( mVertexProgramBound && !mClipPlanes.empty() )
            mClipPlanesDirty = true;

        mVertexProgramBound             = false;
        mGeometryProgramBound           = false;
        mFragmentProgramBound           = false;
        mTessellationHullProgramBound   = false;
        mTessellationDomainProgramBound = false;
        mComputeProgramBound            = false;

        //Derived class must set new state
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setTextureUnitSettings(size_t texUnit, TextureUnitState& tl)
    {
        // This method is only ever called to set a texture unit to valid details
        // The method _disableTextureUnit is called to turn a unit off

        TextureGpu *tex = tl._getTexturePtr();
        bool isValidBinding = false;
        
        if (mCurrentCapabilities->hasCapability(RSC_COMPLETE_TEXTURE_BINDING))
            _setBindingType(tl.getBindingType());

        // Vertex texture binding?
        if (mCurrentCapabilities->hasCapability(RSC_VERTEX_TEXTURE_FETCH) &&
            !mCurrentCapabilities->getVertexTextureUnitsShared())
        {
            isValidBinding = true;
            if (tl.getBindingType() == TextureUnitState::BT_VERTEX)
            {
                // Bind vertex texture
                _setVertexTexture(texUnit, tex);
                // bind nothing to fragment unit (hardware isn't shared but fragment
                // unit can't be using the same index
                _setTexture( texUnit, 0 );
            }
            else
            {
                // vice versa
                _setVertexTexture(texUnit, 0);
                _setTexture( texUnit, tex );
            }
        }

        if (mCurrentCapabilities->hasCapability(RSC_GEOMETRY_PROGRAM))
        {
            isValidBinding = true;
            if (tl.getBindingType() == TextureUnitState::BT_GEOMETRY)
            {
                // Bind vertex texture
                _setGeometryTexture(texUnit, tex);
                // bind nothing to fragment unit (hardware isn't shared but fragment
                // unit can't be using the same index
                _setTexture(texUnit, 0);
            }
            else
            {
                // vice versa
                _setGeometryTexture(texUnit, 0);
                _setTexture(texUnit, tex);
            }
        }

        if (mCurrentCapabilities->hasCapability(RSC_TESSELLATION_DOMAIN_PROGRAM))
        {
            isValidBinding = true;
            if (tl.getBindingType() == TextureUnitState::BT_TESSELLATION_DOMAIN)
            {
                // Bind vertex texture
                _setTessellationDomainTexture(texUnit, tex);
                // bind nothing to fragment unit (hardware isn't shared but fragment
                // unit can't be using the same index
                _setTexture(texUnit, 0);
            }
            else
            {
                // vice versa
                _setTessellationDomainTexture(texUnit, 0);
                _setTexture(texUnit, tex);
            }
        }

        if (mCurrentCapabilities->hasCapability(RSC_TESSELLATION_HULL_PROGRAM))
        {
            isValidBinding = true;
            if (tl.getBindingType() == TextureUnitState::BT_TESSELLATION_HULL)
            {
                // Bind vertex texture
                _setTessellationHullTexture(texUnit, tex);
                // bind nothing to fragment unit (hardware isn't shared but fragment
                // unit can't be using the same index
                _setTexture(texUnit, 0);
            }
            else
            {
                // vice versa
                _setTessellationHullTexture(texUnit, 0);
                _setTexture(texUnit, tex);
            }
        }

        if (!isValidBinding)
        {
            // Shared vertex / fragment textures or no vertex texture support
            // Bind texture (may be blank)
            _setTexture(texUnit, tex);
        }

        _setHlmsSamplerblock( texUnit, tl.getSamplerblock() );

        // Set blend modes
        // Note, colour before alpha is important
        _setTextureBlendMode(texUnit, tl.getColourBlendMode());
        _setTextureBlendMode(texUnit, tl.getAlphaBlendMode());

        // Set texture effects
        TextureUnitState::EffectMap::iterator effi;
        // Iterate over new effects
        bool anyCalcs = false;
        for (effi = tl.mEffects.begin(); effi != tl.mEffects.end(); ++effi)
        {
            switch (effi->second.type)
            {
            case TextureUnitState::ET_ENVIRONMENT_MAP:
                if (effi->second.subtype == TextureUnitState::ENV_CURVED)
                {
                    _setTextureCoordCalculation(texUnit, TEXCALC_ENVIRONMENT_MAP);
                    anyCalcs = true;
                }
                else if (effi->second.subtype == TextureUnitState::ENV_PLANAR)
                {
                    _setTextureCoordCalculation(texUnit, TEXCALC_ENVIRONMENT_MAP_PLANAR);
                    anyCalcs = true;
                }
                else if (effi->second.subtype == TextureUnitState::ENV_REFLECTION)
                {
                    _setTextureCoordCalculation(texUnit, TEXCALC_ENVIRONMENT_MAP_REFLECTION);
                    anyCalcs = true;
                }
                else if (effi->second.subtype == TextureUnitState::ENV_NORMAL)
                {
                    _setTextureCoordCalculation(texUnit, TEXCALC_ENVIRONMENT_MAP_NORMAL);
                    anyCalcs = true;
                }
                break;
            case TextureUnitState::ET_UVSCROLL:
            case TextureUnitState::ET_USCROLL:
            case TextureUnitState::ET_VSCROLL:
            case TextureUnitState::ET_ROTATE:
            case TextureUnitState::ET_TRANSFORM:
                break;
            case TextureUnitState::ET_PROJECTIVE_TEXTURE:
                _setTextureCoordCalculation(texUnit, TEXCALC_PROJECTIVE_TEXTURE, 
                    effi->second.frustum);
                anyCalcs = true;
                break;
            }
        }
        // Ensure any previous texcoord calc settings are reset if there are now none
        if (!anyCalcs)
        {
            _setTextureCoordCalculation(texUnit, TEXCALC_NONE);
        }

        // Change tetxure matrix 
        _setTextureMatrix(texUnit, tl.getTextureTransform());


    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setBindingType(TextureUnitState::BindingType bindingType)
    {
        OGRE_EXCEPT(Exception::ERR_NOT_IMPLEMENTED, 
            "This rendersystem does not support binding texture to other shaders then fragment", 
            "RenderSystem::_setBindingType");
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setVertexTexture(size_t unit, TextureGpu *tex)
    {
        OGRE_EXCEPT(Exception::ERR_NOT_IMPLEMENTED, 
            "This rendersystem does not support separate vertex texture samplers, "
            "you should use the regular texture samplers which are shared between "
            "the vertex and fragment units.", 
            "RenderSystem::_setVertexTexture");
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setGeometryTexture(size_t unit, TextureGpu *tex)
    {
        OGRE_EXCEPT(Exception::ERR_NOT_IMPLEMENTED, 
            "This rendersystem does not support separate geometry texture samplers, "
            "you should use the regular texture samplers which are shared between "
            "the vertex and fragment units.", 
            "RenderSystem::_setGeometryTexture");
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setTessellationHullTexture(size_t unit, TextureGpu *tex)
    {
        OGRE_EXCEPT(Exception::ERR_NOT_IMPLEMENTED, 
            "This rendersystem does not support separate tessellation hull texture samplers, "
            "you should use the regular texture samplers which are shared between "
            "the vertex and fragment units.", 
            "RenderSystem::_setTessellationHullTexture");
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setTessellationDomainTexture(size_t unit, TextureGpu *tex)
    {
        OGRE_EXCEPT(Exception::ERR_NOT_IMPLEMENTED, 
            "This rendersystem does not support separate tessellation domain texture samplers, "
            "you should use the regular texture samplers which are shared between "
            "the vertex and fragment units.", 
            "RenderSystem::_setTessellationDomainTexture");
    }
    //-----------------------------------------------------------------------
    void RenderSystem::destroyRenderPassDescriptor( RenderPassDescriptor *renderPassDesc )
    {
        RenderPassDescriptorSet::iterator itor = mRenderPassDescs.find( renderPassDesc );
        assert( itor != mRenderPassDescs.end() && "Already destroyed?" );
        if( itor != mRenderPassDescs.end() )
            mRenderPassDescs.erase( itor );

        if( renderPassDesc->mDepth.texture )
        {
            _dereferenceSharedDepthBuffer( renderPassDesc->mDepth.texture );

            if( renderPassDesc->mStencil.texture &&
                renderPassDesc->mStencil.texture == renderPassDesc->mDepth.texture )
            {
                _dereferenceSharedDepthBuffer( renderPassDesc->mStencil.texture );
                renderPassDesc->mStencil.texture = 0;
            }
        }
        if( renderPassDesc->mStencil.texture )
        {
            _dereferenceSharedDepthBuffer( renderPassDesc->mStencil.texture );
            renderPassDesc->mStencil.texture = 0;
        }

        delete renderPassDesc;
    }
    //---------------------------------------------------------------------
    void RenderSystem::destroyAllRenderPassDescriptors(void)
    {
        RenderPassDescriptorSet::const_iterator itor = mRenderPassDescs.begin();
        RenderPassDescriptorSet::const_iterator end  = mRenderPassDescs.end();

        while( itor != end )
            delete *itor++;

        mRenderPassDescs.clear();
    }
    //---------------------------------------------------------------------
    void RenderSystem::beginRenderPassDescriptor( RenderPassDescriptor *desc,
                                                  TextureGpu *anyTarget,
                                                  uint8 mipLevel,
                                                  const Vector4 *viewportSizes,
                                                  const Vector4 *scissors,
                                                  uint32 numViewports,
                                                  bool overlaysEnabled,
                                                  bool warnIfRtvWasFlushed )
    {
        assert( anyTarget );

        mCurrentRenderPassDescriptor = desc;
        for( size_t i=0; i<numViewports; ++i )
        {
            mCurrentRenderViewport[i].setDimensions( anyTarget, viewportSizes[i],
                                                     scissors[i], mipLevel );
            mCurrentRenderViewport[i].setOverlaysEnabled( overlaysEnabled );
        }

        mMaxBoundViewports = numViewports;
    }
    //---------------------------------------------------------------------
    void RenderSystem::executeRenderPassDescriptorDelayedActions(void)
    {
    }
    //---------------------------------------------------------------------
    void RenderSystem::endRenderPassDescriptor(void)
    {
        mCurrentRenderPassDescriptor = 0;
        const size_t maxBoundViewports = mMaxBoundViewports;
        for( size_t i=0; i<maxBoundViewports; ++i )
            mCurrentRenderViewport[i].setDimensions( 0, Vector4::ZERO, Vector4::ZERO, 0u );
        mMaxBoundViewports = 1u;

        //Where graphics ends, compute may start, or a new frame.
        //Very likely we'll have to flush the UAVs again, so assume we need.
        mUavRenderingDirty = true;
    }
    //---------------------------------------------------------------------
    void RenderSystem::destroySharedDepthBuffer( TextureGpu *depthBuffer )
    {
        TextureGpuVec &bufferVec = mDepthBufferPool2[depthBuffer->getDepthBufferPoolId()];
        TextureGpuVec::iterator itor = std::find( bufferVec.begin(), bufferVec.end(), depthBuffer );
        if( itor != bufferVec.end() )
        {
            efficientVectorRemove( bufferVec, itor );
            mTextureGpuManager->destroyTexture( depthBuffer );
        }
    }
    //---------------------------------------------------------------------
    void RenderSystem::_cleanupDepthBuffers( void )
    {
        TextureGpuSet::const_iterator itor = mSharedDepthBufferZeroRefCandidates.begin();
        TextureGpuSet::const_iterator endt = mSharedDepthBufferZeroRefCandidates.end();

        while( itor != endt )
        {
            // When a shared depth buffer ends up in mSharedDepthBufferZeroRefCandidates,
            // it's because its ref. count reached 0. However it may have been reacquired.
            // We need to check its reference count is still 0 before deleting it.
            DepthBufferRefMap::iterator itMap = mSharedDepthBufferRefs.find( *itor );
            if( itMap != mSharedDepthBufferRefs.end() && itMap->second == 0u )
            {
                destroySharedDepthBuffer( *itor );
                mSharedDepthBufferRefs.erase( itMap );
            }
            ++itor;
        }

        mSharedDepthBufferZeroRefCandidates.clear();
    }
    //---------------------------------------------------------------------
    void RenderSystem::referenceSharedDepthBuffer( TextureGpu *depthBuffer )
    {
        OGRE_ASSERT_MEDIUM( depthBuffer->getSourceType() == TextureSourceType::SharedDepthBuffer );
        OGRE_ASSERT_MEDIUM( mSharedDepthBufferRefs.find( depthBuffer ) != mSharedDepthBufferRefs.end() );
        ++mSharedDepthBufferRefs[depthBuffer];
    }
    //---------------------------------------------------------------------
    void RenderSystem::_dereferenceSharedDepthBuffer( TextureGpu *depthBuffer )
    {
        if( !depthBuffer )
            return;

        DepthBufferRefMap::iterator itor = mSharedDepthBufferRefs.find( depthBuffer );

        if( itor != mSharedDepthBufferRefs.end() )
        {
            OGRE_ASSERT_MEDIUM( depthBuffer->getSourceType() == TextureSourceType::SharedDepthBuffer );
            OGRE_ASSERT_LOW( itor->second > 0u && "Releasing a shared depth buffer too much" );
            --itor->second;

            if( itor->second == 0u )
                mSharedDepthBufferZeroRefCandidates.insert( depthBuffer );
        }
        else
        {
            // This is not a shared depth buffer (e.g. one created by the user)
            OGRE_ASSERT_MEDIUM( depthBuffer->getSourceType() != TextureSourceType::SharedDepthBuffer );
        }
    }
    //---------------------------------------------------------------------
    TextureGpu* RenderSystem::createDepthBufferFor( TextureGpu *colourTexture, bool preferDepthTexture,
                                                    PixelFormatGpu depthBufferFormat, uint16 poolId )
    {
        uint32 textureFlags = TextureFlags::RenderToTexture;

        if( !preferDepthTexture )
            textureFlags |= TextureFlags::NotTexture;

        char tmpBuffer[64];
        LwString depthBufferName( LwString::FromEmptyPointer( tmpBuffer, sizeof(tmpBuffer) ) );
        depthBufferName.a( "DepthBuffer_", Id::generateNewId<TextureGpu>() );

        TextureGpu *retVal = mTextureGpuManager->createTexture( depthBufferName.c_str(),
                                                                GpuPageOutStrategy::Discard,
                                                                textureFlags, TextureTypes::Type2D );
        retVal->setResolution( colourTexture->getWidth(), colourTexture->getHeight() );
        retVal->setPixelFormat( depthBufferFormat );
        retVal->_setDepthBufferDefaults( poolId, preferDepthTexture, depthBufferFormat );
        retVal->_setSourceType( TextureSourceType::SharedDepthBuffer );
        retVal->setSampleDescription( colourTexture->getRequestedSampleDescription() );

        retVal->_transitionTo( GpuResidency::Resident, (uint8*)0 );

        // Start reference count on the depth buffer here
        mSharedDepthBufferRefs[retVal] = 1u;
        return retVal;
    }
    //---------------------------------------------------------------------
    TextureGpu* RenderSystem::getDepthBufferFor( TextureGpu *colourTexture, uint16 poolId,
                                                 bool preferDepthTexture,
                                                 PixelFormatGpu depthBufferFormat )
    {
        if( poolId == DepthBuffer::POOL_NO_DEPTH || depthBufferFormat == PFG_NULL )
            return 0; //RenderTarget explicitly requested no depth buffer

        if( colourTexture->isRenderWindowSpecific() )
        {
            Window *window;
            colourTexture->getCustomAttribute( "Window", &window );
            return window->getDepthBuffer();
        }

        if( poolId == DepthBuffer::POOL_NON_SHAREABLE )
        {
            TextureGpu *retVal = createDepthBufferFor( colourTexture, preferDepthTexture,
                                                       depthBufferFormat, poolId );
            return retVal;
        }

        //Find a depth buffer in the pool
        TextureGpuVec::const_iterator itor = mDepthBufferPool2[poolId].begin();
        TextureGpuVec::const_iterator end  = mDepthBufferPool2[poolId].end();

        TextureGpu *retVal = 0;

        while( itor != end && !retVal )
        {
            if( preferDepthTexture == (*itor)->isTexture() &&
                (depthBufferFormat == PFG_UNKNOWN ||
                 depthBufferFormat == (*itor)->getPixelFormat()) &&
                (*itor)->supportsAsDepthBufferFor( colourTexture ) )
            {
                retVal = *itor;
                referenceSharedDepthBuffer( retVal );
            }
            else
            {
                retVal = 0;
            }
            ++itor;
        }

        //Not found yet? Create a new one!
        if( !retVal )
        {
            retVal =
                createDepthBufferFor( colourTexture, preferDepthTexture, depthBufferFormat, poolId );
            mDepthBufferPool2[poolId].push_back( retVal );

            if( !retVal )
            {
                LogManager::getSingleton().logMessage( "WARNING: Couldn't create a suited "
                                                       "DepthBuffer for RTT: " +
                                                       colourTexture->getNameStr(), LML_CRITICAL );
            }
        }

        return retVal;
    }
    //---------------------------------------------------------------------
    void RenderSystem::setUavStartingSlot( uint32 startingSlot )
    {
        mUavStartingSlot = startingSlot;
        mUavRenderingDirty = true;
    }
    //---------------------------------------------------------------------
    void RenderSystem::queueBindUAVs( const DescriptorSetUav *descSetUav )
    {
        if( mUavRenderingDescSet != descSetUav )
        {
            mUavRenderingDescSet = descSetUav;
            mUavRenderingDirty = true;
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_beginFrameOnce(void)
    {
        mVaoManager->_beginFrame();
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_endFrameOnce(void)
    {
        queueBindUAVs( 0 );
    }
    //-----------------------------------------------------------------------
    bool RenderSystem::getWBufferEnabled(void) const
    {
        return mWBuffer;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::setWBufferEnabled(bool enabled)
    {
        mWBuffer = enabled;
    }
    //-----------------------------------------------------------------------
    SampleDescription RenderSystem::validateSampleDescription( const SampleDescription &sampleDesc,
                                                               PixelFormatGpu format )
    {
        SampleDescription retVal( sampleDesc.getMaxSamples(), sampleDesc.getMsaaPattern() );
        return retVal;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::shutdown(void)
    {
        // Remove occlusion queries
        for (HardwareOcclusionQueryList::iterator i = mHwOcclusionQueries.begin();
            i != mHwOcclusionQueries.end(); ++i)
        {
            OGRE_DELETE *i;
        }
        mHwOcclusionQueries.clear();

        destroyAllRenderPassDescriptors();
        _cleanupDepthBuffers();
        OGRE_ASSERT_LOW( mSharedDepthBufferRefs.empty() &&
                         "destroyAllRenderPassDescriptors followed by _cleanupDepthBuffers should've "
                         "emptied mSharedDepthBufferRefs. Please report this bug to "
                         "https://github.com/OGRECave/ogre-next/issues/" );

        OGRE_DELETE mTextureGpuManager;
        mTextureGpuManager = 0;
        OGRE_DELETE mVaoManager;
        mVaoManager = 0;

        {
            // Remove all windows.
            // (destroy primary window last since others may depend on it)
            Window *primary = 0;
            WindowSet::const_iterator itor = mWindows.begin();
            WindowSet::const_iterator end  = mWindows.end();

            while( itor != end )
            {
                //Set mTextureManager to 0 as it is no longer valid on shutdown
                if( (*itor)->getTexture() )
                    (*itor)->getTexture()->_resetTextureManager();
                if( (*itor)->getDepthBuffer() )
                    (*itor)->getDepthBuffer()->_resetTextureManager();
                if( (*itor)->getStencilBuffer() )
                    (*itor)->getStencilBuffer()->_resetTextureManager();

                if( !primary && (*itor)->isPrimary() )
                    primary = *itor;
                else
                    OGRE_DELETE *itor;

                ++itor;
            }

            OGRE_DELETE primary;
            mWindows.clear();
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_resetMetrics()
    {
        const bool oldValue = mMetrics.mIsRecordingMetrics;
        mMetrics = Metrics();
        mMetrics.mIsRecordingMetrics = oldValue;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_addMetrics( const Metrics &newMetrics )
    {
        if( mMetrics.mIsRecordingMetrics )
        {
            mMetrics.mBatchCount += newMetrics.mBatchCount;
            mMetrics.mFaceCount += newMetrics.mFaceCount;
            mMetrics.mVertexCount += newMetrics.mVertexCount;
            mMetrics.mDrawCount += newMetrics.mDrawCount;
            mMetrics.mInstanceCount += newMetrics.mInstanceCount;
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::setMetricsRecordingEnabled( bool bEnable )
    {
        mMetrics.mIsRecordingMetrics = bEnable;
    }
    //-----------------------------------------------------------------------
    const RenderSystem::Metrics& RenderSystem::getMetrics() const
    {
        return mMetrics;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::convertColourValue(const ColourValue& colour, uint32* pDest)
    {
        *pDest = v1::VertexElement::convertColourValue(colour, getColourVertexElementType());
    }
    //-----------------------------------------------------------------------
    CompareFunction RenderSystem::reverseCompareFunction( CompareFunction depthFunc )
    {
        switch( depthFunc )
        {
        case CMPF_LESS:         return CMPF_GREATER;
        case CMPF_LESS_EQUAL:   return CMPF_GREATER_EQUAL;
        case CMPF_GREATER_EQUAL:return CMPF_LESS_EQUAL;
        case CMPF_GREATER:      return CMPF_LESS;
        default:                return depthFunc;
        }

        return depthFunc;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_makeRsProjectionMatrix( const Matrix4& matrix,
                                                Matrix4& dest, Real nearPlane,
                                                Real farPlane, ProjectionType projectionType )
    {
        dest = matrix;

        Real inv_d = 1 / (farPlane - nearPlane);
        Real q, qn;

        if( mReverseDepth )
        {
            if( projectionType == PT_PERSPECTIVE )
            {
                if( farPlane == 0 )
                {
                    // Infinite far plane
                    //  q   = limit( near / (far - near), far, inf );
                    //  qn  = limit( (far * near) / (far - near), far, inf );
                    q   = 0;
                    qn  = nearPlane;
                }
                else
                {
                    //Standard Z for range [-1; 1]
                    //  q = - (far + near) / (far - near)
                    //  qn = - 2 * (far * near) / (far - near)
                    //
                    //Standard Z for range [0; 1]
                    //  q = - far / (far - near)
                    //  qn = - (far * near) / (far - near)
                    //
                    //Reverse Z for range [1; 0]:
                    // [ 1   0    0  0  ]   [ A   0   C   0  ]
                    // [ 0   1    0  0  ] X [ 0   B   D   0  ]
                    // [ 0   0   -1  1  ]   [ 0   0   q   qn ]
                    // [ 0   0    0  1  ]   [ 0   0   -1  0  ]
                    //
                    // [ A   0   C      0  ]
                    // [ 0   B   D      0  ]
                    // [ 0   0   -q-1  -qn ]
                    // [ 0   0   -1     0  ]
                    //
                    //  q' = -q - 1
                    //     =  far / (far - near) - 1
                    //     = ( far - (far - near) ) / (far - near)
                    //  q' = near / (far - near)
                    //  qn'= -qn
                    q   = nearPlane * inv_d;
                    qn  = (farPlane * nearPlane) * inv_d;
                }
            }
            else
            {
                if( farPlane == 0 )
                {
                    // Can not do infinite far plane here, avoid divided zero only
                    q = Frustum::INFINITE_FAR_PLANE_ADJUST / nearPlane;
                    qn = Frustum::INFINITE_FAR_PLANE_ADJUST + 1;
                }
                else
                {
                    //Standard Z for range [-1; 1]
                    //  q = - 2 / (far - near)
                    //  qn = -(far + near) / (far - near)
                    //
                    //Standard Z for range [0; 1]
                    //  q = - 1 / (far - near)
                    //  qn = - near / (far - near)
                    //
                    //Reverse Z for range [1; 0]:
                    //  q' = 1 / (far - near)
                    //  qn'= far / (far - near)
                    q   = inv_d;
                    qn  = farPlane * inv_d;
                }
            }
        }
        else
        {
            if( projectionType == PT_PERSPECTIVE )
            {
                if( farPlane == 0 )
                {
                    // Infinite far plane
                    q   = Frustum::INFINITE_FAR_PLANE_ADJUST - 1;
                    qn  = nearPlane * (Frustum::INFINITE_FAR_PLANE_ADJUST - 1);
                }
                else
                {
                    q   = -farPlane * inv_d;
                    qn  = -(farPlane * nearPlane) * inv_d;
                }
            }
            else
            {
                if( farPlane == 0 )
                {
                    // Can not do infinite far plane here, avoid divided zero only
                    q   = -Frustum::INFINITE_FAR_PLANE_ADJUST / nearPlane;
                    qn  = -Frustum::INFINITE_FAR_PLANE_ADJUST;
                }
                else
                {
                    q   = -inv_d;
                    qn  = -nearPlane * inv_d;
                }
            }
        }

        dest[2][2] = q;
        dest[2][3] = qn;
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_convertProjectionMatrix( const Matrix4& matrix, Matrix4& dest )
    {
        dest = matrix;

        if( !mReverseDepth )
        {
            // Convert depth range from [-1,+1] to [0,1]
            dest[2][0] = (dest[2][0] + dest[3][0]) / 2;
            dest[2][1] = (dest[2][1] + dest[3][1]) / 2;
            dest[2][2] = (dest[2][2] + dest[3][2]) / 2;
            dest[2][3] = (dest[2][3] + dest[3][3]) / 2;
        }
        else
        {
            // Convert depth range from [-1,+1] to [1,0]
            dest[2][0] = (-dest[2][0] + dest[3][0]) / 2;
            dest[2][1] = (-dest[2][1] + dest[3][1]) / 2;
            dest[2][2] = (-dest[2][2] + dest[3][2]) / 2;
            dest[2][3] = (-dest[2][3] + dest[3][3]) / 2;
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_convertOpenVrProjectionMatrix( const Matrix4& matrix, Matrix4& dest )
    {
        dest = matrix;

        if( mReverseDepth )
        {
            // Convert depth range from [0,1] to [1,0]
            dest[2][0] = (-dest[2][0] + dest[3][0]);
            dest[2][1] = (-dest[2][1] + dest[3][1]);
            dest[2][2] = (-dest[2][2] + dest[3][2]);
            dest[2][3] = (-dest[2][3] + dest[3][3]);
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_setWorldMatrices(const Matrix4* m, unsigned short count)
    {
        // Do nothing with these matrices here, it never used for now,
        // derived class should take care with them if required.

        // Set hardware matrix to nothing
        _setWorldMatrix(Matrix4::IDENTITY);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::setStencilBufferParams( uint32 refValue, const StencilParams &stencilParams )
    {
        mStencilParams = stencilParams;

        // NB: We should always treat CCW as front face for consistent with default
        // culling mode.
        const bool mustFlip =
                ((mInvertVertexWinding && !mCurrentRenderPassDescriptor->requiresTextureFlipping()) ||
                 (!mInvertVertexWinding && mCurrentRenderPassDescriptor->requiresTextureFlipping()));

        if( mustFlip )
        {
            mStencilParams.stencilBack = stencilParams.stencilFront;
            mStencilParams.stencilFront = stencilParams.stencilBack;
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::_render(const v1::RenderOperation& op)
    {
        // Update stats
        size_t val;

        if (op.useIndexes)
            val = op.indexData->indexCount;
        else
            val = op.vertexData->vertexCount;

        size_t trueInstanceNum = std::max<size_t>(op.numberOfInstances,1);
        val *= trueInstanceNum;

        // account for a pass having multiple iterations
        if (mCurrentPassIterationCount > 1)
            val *= mCurrentPassIterationCount;
        mCurrentPassIterationNum = 0;

        switch(op.operationType)
        {
        case OT_TRIANGLE_LIST:
            mMetrics.mFaceCount += (val / 3u);
            break;
        case OT_TRIANGLE_STRIP:
        case OT_TRIANGLE_FAN:
            mMetrics.mFaceCount += (val - 2u);
            break;
        case OT_POINT_LIST:
        case OT_LINE_LIST:
        case OT_LINE_STRIP:
        case OT_PATCH_1_CONTROL_POINT:
        case OT_PATCH_2_CONTROL_POINT:
        case OT_PATCH_3_CONTROL_POINT:
        case OT_PATCH_4_CONTROL_POINT:
        case OT_PATCH_5_CONTROL_POINT:
        case OT_PATCH_6_CONTROL_POINT:
        case OT_PATCH_7_CONTROL_POINT:
        case OT_PATCH_8_CONTROL_POINT:
        case OT_PATCH_9_CONTROL_POINT:
        case OT_PATCH_10_CONTROL_POINT:
        case OT_PATCH_11_CONTROL_POINT:
        case OT_PATCH_12_CONTROL_POINT:
        case OT_PATCH_13_CONTROL_POINT:
        case OT_PATCH_14_CONTROL_POINT:
        case OT_PATCH_15_CONTROL_POINT:
        case OT_PATCH_16_CONTROL_POINT:
        case OT_PATCH_17_CONTROL_POINT:
        case OT_PATCH_18_CONTROL_POINT:
        case OT_PATCH_19_CONTROL_POINT:
        case OT_PATCH_20_CONTROL_POINT:
        case OT_PATCH_21_CONTROL_POINT:
        case OT_PATCH_22_CONTROL_POINT:
        case OT_PATCH_23_CONTROL_POINT:
        case OT_PATCH_24_CONTROL_POINT:
        case OT_PATCH_25_CONTROL_POINT:
        case OT_PATCH_26_CONTROL_POINT:
        case OT_PATCH_27_CONTROL_POINT:
        case OT_PATCH_28_CONTROL_POINT:
        case OT_PATCH_29_CONTROL_POINT:
        case OT_PATCH_30_CONTROL_POINT:
        case OT_PATCH_31_CONTROL_POINT:
        case OT_PATCH_32_CONTROL_POINT:
            break;
        }

        mMetrics.mVertexCount += op.vertexData->vertexCount * trueInstanceNum;
        mMetrics.mBatchCount += mCurrentPassIterationCount;

        // sort out clip planes
        // have to do it here in case of matrix issues
        if (mClipPlanesDirty)
        {
            setClipPlanesImpl(mClipPlanes);
            mClipPlanesDirty = false;
        }
    }
    //-----------------------------------------------------------------------
    /*void RenderSystem::_render( const VertexArrayObject *vao )
    {
        // Update stats
        mFaceCount      += vao->mFaceCount;
        mVertexCount    += vao->mVertexBuffers[0]->getNumElements();
        ++mBatchCount;
    }*/
    //-----------------------------------------------------------------------
    void RenderSystem::setInvertVertexWinding(bool invert)
    {
        mInvertVertexWinding = invert;
    }
    //-----------------------------------------------------------------------
    bool RenderSystem::getInvertVertexWinding(void) const
    {
        return mInvertVertexWinding;
    }
    //---------------------------------------------------------------------
    void RenderSystem::addClipPlane (const Plane &p)
    {
        mClipPlanes.push_back(p);
        mClipPlanesDirty = true;
    }
    //---------------------------------------------------------------------
    void RenderSystem::addClipPlane (Real A, Real B, Real C, Real D)
    {
        addClipPlane(Plane(A, B, C, D));
    }
    //---------------------------------------------------------------------
    void RenderSystem::setClipPlanes(const PlaneList& clipPlanes)
    {
        if (clipPlanes != mClipPlanes)
        {
            mClipPlanes = clipPlanes;
            mClipPlanesDirty = true;
        }
    }
    //---------------------------------------------------------------------
    void RenderSystem::resetClipPlanes()
    {
        if (!mClipPlanes.empty())
        {
            mClipPlanes.clear();
            mClipPlanesDirty = true;
        }
    }
    //---------------------------------------------------------------------
    bool RenderSystem::updatePassIterationRenderState(void)
    {
        if (mCurrentPassIterationCount <= 1)
            return false;

        --mCurrentPassIterationCount;
        ++mCurrentPassIterationNum;
        if (!mActiveVertexGpuProgramParameters.isNull())
        {
            mActiveVertexGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_VERTEX_PROGRAM);
        }
        if (!mActiveGeometryGpuProgramParameters.isNull())
        {
            mActiveGeometryGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_GEOMETRY_PROGRAM);
        }
        if (!mActiveFragmentGpuProgramParameters.isNull())
        {
            mActiveFragmentGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_FRAGMENT_PROGRAM);
        }
        if (!mActiveTessellationHullGpuProgramParameters.isNull())
        {
            mActiveTessellationHullGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_HULL_PROGRAM);
        }
        if (!mActiveTessellationDomainGpuProgramParameters.isNull())
        {
            mActiveTessellationDomainGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_DOMAIN_PROGRAM);
        }
        if (!mActiveComputeGpuProgramParameters.isNull())
        {
            mActiveComputeGpuProgramParameters->incPassIterationNumber();
            bindGpuProgramPassIterationParameters(GPT_COMPUTE_PROGRAM);
        }
        return true;
    }

    //-----------------------------------------------------------------------
    void RenderSystem::addSharedListener(Listener* l)
    {
        msSharedEventListeners.push_back(l);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::removeSharedListener(Listener* l)
    {
        msSharedEventListeners.remove(l);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::addListener(Listener* l)
    {
        mEventListeners.push_back(l);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::removeListener(Listener* l)
    {
        mEventListeners.remove(l);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::fireEvent(const String& name, const NameValuePairList* params)
    {
        for(ListenerList::iterator i = mEventListeners.begin(); 
            i != mEventListeners.end(); ++i)
        {
            (*i)->eventOccurred(name, params);
        }
        fireSharedEvent(name, params);
    }
    //-----------------------------------------------------------------------
    void RenderSystem::fireSharedEvent(const String& name, const NameValuePairList* params)
    {
        for(ListenerList::iterator i = msSharedEventListeners.begin(); 
            i != msSharedEventListeners.end(); ++i)
        {
            (*i)->eventOccurred(name, params);
        }
    }
    //-----------------------------------------------------------------------
    void RenderSystem::destroyHardwareOcclusionQuery( HardwareOcclusionQuery *hq)
    {
        HardwareOcclusionQueryList::iterator i =
            std::find(mHwOcclusionQueries.begin(), mHwOcclusionQueries.end(), hq);
        if (i != mHwOcclusionQueries.end())
        {
            mHwOcclusionQueries.erase(i);
            OGRE_DELETE hq;
        }
    }
    //-----------------------------------------------------------------------
    bool RenderSystem::isGpuProgramBound(GpuProgramType gptype)
    {
        switch(gptype)
        {
        case GPT_VERTEX_PROGRAM:
            return mVertexProgramBound;
        case GPT_GEOMETRY_PROGRAM:
            return mGeometryProgramBound;
        case GPT_FRAGMENT_PROGRAM:
            return mFragmentProgramBound;
        case GPT_HULL_PROGRAM:
            return mTessellationHullProgramBound;
        case GPT_DOMAIN_PROGRAM:
            return mTessellationDomainProgramBound;
        case GPT_COMPUTE_PROGRAM:
            return mComputeProgramBound;
        }
        // Make compiler happy
        return false;
    }
    //---------------------------------------------------------------------
    void RenderSystem::_setTextureProjectionRelativeTo(bool enabled, const Vector3& pos)
    {
        mTexProjRelative = enabled;
        mTexProjRelativeOrigin = pos;

    }
    //---------------------------------------------------------------------
    RenderSystem::RenderSystemContext* RenderSystem::_pauseFrame(void)
    {
        _endFrame();
        return new RenderSystem::RenderSystemContext;
    }
    //---------------------------------------------------------------------
    void RenderSystem::_resumeFrame(RenderSystemContext* context)
    {
        _beginFrame();
        delete context;
    }
    //---------------------------------------------------------------------
    void RenderSystem::_update(void)
    {
        OgreProfile( "RenderSystem::_update" );
        mTextureGpuManager->_update( false );
        mVaoManager->_update();
    }
    //---------------------------------------------------------------------
    void RenderSystem::updateCompositorManager( CompositorManager2 *compositorManager )
    {
        compositorManager->_updateImplementation();
    }
    //---------------------------------------------------------------------
    const String& RenderSystem::_getDefaultViewportMaterialScheme( void ) const
    {
#ifdef RTSHADER_SYSTEM_BUILD_CORE_SHADERS   
        if ( !(getCapabilities()->hasCapability(Ogre::RSC_FIXED_FUNCTION)) )
        {
            // I am returning the exact value for now - I don't want to add dependency for the RTSS just for one string  
            static const String ShaderGeneratorDefaultScheme = "ShaderGeneratorDefaultScheme";
            return ShaderGeneratorDefaultScheme;
        }
        else
#endif
        {
            return MaterialManager::DEFAULT_SCHEME_NAME;
        }
    }
    //---------------------------------------------------------------------
    Ogre::v1::HardwareVertexBufferSharedPtr RenderSystem::getGlobalInstanceVertexBuffer() const
    {
        return mGlobalInstanceVertexBuffer;
    }
    //---------------------------------------------------------------------
    void RenderSystem::setGlobalInstanceVertexBuffer( const v1::HardwareVertexBufferSharedPtr &val )
    {
        if ( !val.isNull() && !val->getIsInstanceData() )
        {
            OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, 
                        "A none instance data vertex buffer was set to be the global instance vertex buffer.",
                        "RenderSystem::setGlobalInstanceVertexBuffer");
        }
        mGlobalInstanceVertexBuffer = val;
    }
    //---------------------------------------------------------------------
    size_t RenderSystem::getGlobalNumberOfInstances() const
    {
        return mGlobalNumberOfInstances;
    }
    //---------------------------------------------------------------------
    void RenderSystem::setGlobalNumberOfInstances( const size_t val )
    {
        mGlobalNumberOfInstances = val;
    }

    v1::VertexDeclaration* RenderSystem::getGlobalInstanceVertexBufferVertexDeclaration() const
    {
        return mGlobalInstanceVertexBufferVertexDeclaration;
    }
    //---------------------------------------------------------------------
    void RenderSystem::setGlobalInstanceVertexBufferVertexDeclaration( v1::VertexDeclaration* val )
    {
        mGlobalInstanceVertexBufferVertexDeclaration = val;
    }
    //---------------------------------------------------------------------
    void RenderSystem::getCustomAttribute(const String& name, void* pData)
    {
        OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS, "Attribute not found.", "RenderSystem::getCustomAttribute");
    }
    //---------------------------------------------------------------------
    void RenderSystem::setDebugShaders( bool bDebugShaders )
    {
        mDebugShaders = bDebugShaders;
    }
    //---------------------------------------------------------------------
    void RenderSystem::_clearStateAndFlushCommandBuffer(void)
    {
    }
    //---------------------------------------------------------------------
    RenderSystem::Listener::~Listener() {}
    RenderSystem::Metrics::Metrics() :
        mIsRecordingMetrics( false ),
        mBatchCount( 0 ),
        mFaceCount( 0 ),
        mVertexCount( 0 ),
        mDrawCount( 0 ),
        mInstanceCount( 0 )
    {
    }
}

