//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  RendererD3D12.cpp
//
//  Direct3D 12 renderer, used by the UWP (Xbox Dev Mode) build where OpenGL is not
//  available. It implements the same interface and visual output as RendererOpenGL.
//
//  Coordinate conventions:
//  ES-DE was written for OpenGL. Texture data is uploaded with the first row at texture
//  coordinate v = 0 in both APIs, so regular textures need no changes at all. The only
//  difference is that OpenGL framebuffers have their first row at the bottom of the screen
//  while Direct3D render targets have it at the top. This matters only for the
//  post-processing (where the screen contents are used as a texture) and for reading back
//  pixels, which are both handled in shaderPostprocessing().
//

#if defined(USE_D3D12_RENDERER)

#include "renderers/RendererD3D12.h"

#include "Settings.h"
#include "renderers/ShadersD3D12.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace
{
    constexpr size_t UPLOAD_BUFFER_SIZE {32 * 1024 * 1024};
    constexpr DXGI_FORMAT BACK_BUFFER_FORMAT {DXGI_FORMAT_B8G8R8A8_UNORM};

    size_t alignUp(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    unsigned int bytesPerPixel(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
    }

    std::string hresultString(HRESULT result)
    {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned int>(result));
        return buffer;
    }

    D3D12_RESOURCE_BARRIER transitionBarrier(ID3D12Resource* resource,
                                             D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        return barrier;
    }

    D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = type;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    D3D12_RESOURCE_DESC bufferDesc(UINT64 size)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return desc;
    }

    // Box-filter downsampling used to create mipmaps on the CPU, as Direct3D 12 has no
    // equivalent to glGenerateMipmap().
    std::vector<unsigned char> downsample(const unsigned char* source,
                                          unsigned int width,
                                          unsigned int height,
                                          unsigned int& newWidth,
                                          unsigned int& newHeight)
    {
        newWidth = std::max(1u, width / 2);
        newHeight = std::max(1u, height / 2);
        std::vector<unsigned char> result(static_cast<size_t>(newWidth) * newHeight * 4);

        for (unsigned int y {0}; y < newHeight; ++y) {
            const unsigned int y0 {std::min(y * 2, height - 1)};
            const unsigned int y1 {std::min(y * 2 + 1, height - 1)};
            for (unsigned int x {0}; x < newWidth; ++x) {
                const unsigned int x0 {std::min(x * 2, width - 1)};
                const unsigned int x1 {std::min(x * 2 + 1, width - 1)};
                for (unsigned int c {0}; c < 4; ++c) {
                    const unsigned int sum {
                        static_cast<unsigned int>(source[(y0 * width + x0) * 4 + c]) +
                        source[(y0 * width + x1) * 4 + c] + source[(y1 * width + x0) * 4 + c] +
                        source[(y1 * width + x1) * 4 + c]};
                    result[(static_cast<size_t>(y) * newWidth + x) * 4 + c] =
                        static_cast<unsigned char>((sum + 2) / 4);
                }
            }
        }
        return result;
    }

} // namespace

RendererD3D12::RendererD3D12() noexcept
    : mFenceEvent {nullptr}
    , mNextFenceValue {1}
    , mRTVDescriptorSize {0}
    , mSRVDescriptorSize {0}
    , mSamplerDescriptorSize {0}
    , mBackBufferStates {}
    , mFrameIndex {0}
    , mFrameActive {false}
    , mBoundTexture {0}
    , mWhiteTexture {0}
    , mPostProcTexture1 {0}
    , mPostProcTexture2 {0}
    , mCurrentRTV {}
    , mRenderingToPostProcTarget {false}
    , mD3DViewport {}
    , mD3DScissor {}
    , mScissorEnabled {false}
    , mVSync {true}
    , mTearingSupported {false}
    , mDeviceLost {false}
{
}

RendererD3D12* RendererD3D12::getInstance()
{
    static RendererD3D12 instance;
    return &instance;
}

DXGI_FORMAT RendererD3D12::convertTextureType(const TextureType type)
{
    // clang-format off
    switch (type) {
        case TextureType::RGBA: { return DXGI_FORMAT_R8G8B8A8_UNORM; }
        case TextureType::BGRA: { return DXGI_FORMAT_B8G8R8A8_UNORM; }
        case TextureType::RED:  { return DXGI_FORMAT_R8_UNORM;       }
        default:                { return DXGI_FORMAT_B8G8R8A8_UNORM; }
    }
    // clang-format on
}

D3D12_BLEND RendererD3D12::convertBlendFactor(const BlendFactor blendFactor)
{
    // clang-format off
    switch (blendFactor) {
        case BlendFactor::ZERO:                { return D3D12_BLEND_ZERO;           }
        case BlendFactor::ONE:                 { return D3D12_BLEND_ONE;            }
        case BlendFactor::SRC_COLOR:           { return D3D12_BLEND_SRC_COLOR;      }
        case BlendFactor::ONE_MINUS_SRC_COLOR: { return D3D12_BLEND_INV_SRC_COLOR;  }
        case BlendFactor::SRC_ALPHA:           { return D3D12_BLEND_SRC_ALPHA;      }
        case BlendFactor::ONE_MINUS_SRC_ALPHA: { return D3D12_BLEND_INV_SRC_ALPHA;  }
        case BlendFactor::DST_COLOR:           { return D3D12_BLEND_DEST_COLOR;     }
        case BlendFactor::ONE_MINUS_DST_COLOR: { return D3D12_BLEND_INV_DEST_COLOR; }
        case BlendFactor::DST_ALPHA:           { return D3D12_BLEND_DEST_ALPHA;     }
        case BlendFactor::ONE_MINUS_DST_ALPHA: { return D3D12_BLEND_INV_DEST_ALPHA; }
        default:                               { return D3D12_BLEND_ZERO;           }
    }
    // clang-format on
}

// -----------------------------------------------------------------------------------------
// Setup and teardown.
// -----------------------------------------------------------------------------------------

void RendererD3D12::setup()
{
    // Nothing to configure before the window is created, unlike OpenGL where the context
    // attributes need to be set beforehand.
}

bool RendererD3D12::createDevice()
{
    UINT factoryFlags {0};
#if !defined(NDEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        LOG(LogInfo) << "Direct3D 12 debug layer enabled";
    }
#endif

    HRESULT result {CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&mFactory))};
    if (FAILED(result)) {
        LOG(LogError) << "CreateDXGIFactory2() failed: " << hresultString(result);
        return false;
    }

    result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));
    if (FAILED(result)) {
        LOG(LogError) << "D3D12CreateDevice() failed: " << hresultString(result);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    result = mDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue));
    if (FAILED(result)) {
        LOG(LogError) << "CreateCommandQueue() failed: " << hresultString(result);
        return false;
    }

    for (auto& frame : mFrames) {
        result = mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result)) {
            LOG(LogError) << "CreateCommandAllocator() failed: " << hresultString(result);
            return false;
        }

        const D3D12_HEAP_PROPERTIES uploadHeap {heapProperties(D3D12_HEAP_TYPE_UPLOAD)};
        const D3D12_RESOURCE_DESC desc {bufferDesc(UPLOAD_BUFFER_SIZE)};
        result = mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&frame.uploadBuffer));
        if (FAILED(result)) {
            LOG(LogError) << "Couldn't create upload buffer: " << hresultString(result);
            return false;
        }
        D3D12_RANGE readRange {0, 0};
        frame.uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&frame.uploadData));
        frame.uploadSize = UPLOAD_BUFFER_SIZE;
        frame.uploadOffset = 0;
    }

    result = mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        mFrames[0].allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&mCommandList));
    if (FAILED(result)) {
        LOG(LogError) << "CreateCommandList() failed: " << hresultString(result);
        return false;
    }
    mCommandList->Close();

    result = mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence));
    if (FAILED(result)) {
        LOG(LogError) << "CreateFence() failed: " << hresultString(result);
        return false;
    }
    mFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (mFenceEvent == nullptr) {
        LOG(LogError) << "Couldn't create fence event";
        return false;
    }

    // Descriptor heaps.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT + 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRTVHeap))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc {};
    srvHeapDesc.NumDescriptors = MAX_TEXTURES;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSRVHeap))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc {};
    samplerHeapDesc.NumDescriptors = SAMPLER_COUNT;
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(mDevice->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&mSamplerHeap))))
        return false;

    mRTVDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mSRVDescriptorSize =
        mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    mSamplerDescriptorSize =
        mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // Texture slot 0 is reserved as "no texture", which is mapped to the white texture.
    mTextures.clear();
    mTextures.resize(1);
    mFreeTextureSlots.clear();

    return true;
}

bool RendererD3D12::createSwapChain()
{
    SDL_SysWMinfo info {};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(getSDLWindow(), &info)) {
        LOG(LogError) << "SDL_GetWindowWMInfo() failed: " << SDL_GetError();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = static_cast<UINT>(mWindowWidth);
    desc.Height = static_cast<UINT>(mWindowHeight);
    desc.Format = BACK_BUFFER_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FRAME_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swapChain;
    HRESULT result {E_FAIL};

#if defined(SDL_VIDEO_DRIVER_WINRT)
    if (info.subsystem == SDL_SYSWM_WINRT) {
        LOG(LogInfo) << "Creating Direct3D 12 swap chain for CoreWindow";
        result = mFactory->CreateSwapChainForCoreWindow(
            mCommandQueue.Get(), reinterpret_cast<IUnknown*>(info.info.winrt.window), &desc,
            nullptr, &swapChain);
    }
#endif
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        LOG(LogInfo) << "Creating Direct3D 12 swap chain for HWND";
        result = mFactory->CreateSwapChainForHwnd(mCommandQueue.Get(), info.info.win.window,
                                                  &desc, nullptr, nullptr, &swapChain);
        if (SUCCEEDED(result))
            mFactory->MakeWindowAssociation(info.info.win.window, DXGI_MWA_NO_ALT_ENTER);
    }
#endif

    if (FAILED(result) || !swapChain) {
        LOG(LogError) << "Couldn't create swap chain (SDL window subsystem "
                      << static_cast<int>(info.subsystem) << "): " << hresultString(result);
        return false;
    }

    if (FAILED(swapChain.As(&mSwapChain))) {
        LOG(LogError) << "IDXGISwapChain3 is not supported";
        return false;
    }

    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
    return true;
}

bool RendererD3D12::createRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
    for (unsigned int i {0}; i < FRAME_COUNT; ++i) {
        if (FAILED(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mBackBuffers[i])))) {
            LOG(LogError) << "Couldn't get swap chain buffer " << i;
            return false;
        }
        mDevice->CreateRenderTargetView(mBackBuffers[i].Get(), nullptr, rtvHandle);
        mBackBufferStates[i] = D3D12_RESOURCE_STATE_PRESENT;
        rtvHandle.ptr += mRTVDescriptorSize;
    }
    return true;
}

void RendererD3D12::createSamplers()
{
    // Index = ((minify * 2) + magnify) * 2 + repeat, where minify is 0 = point, 1 = linear,
    // 2 = linear with mipmaps and magnify is 0 = point, 1 = linear.
    D3D12_CPU_DESCRIPTOR_HANDLE handle {mSamplerHeap->GetCPUDescriptorHandleForHeapStart()};
    for (unsigned int minify {0}; minify < 3; ++minify) {
        for (unsigned int magnify {0}; magnify < 2; ++magnify) {
            for (unsigned int repeat {0}; repeat < 2; ++repeat) {
                D3D12_SAMPLER_DESC desc {};
                if (minify == 0)
                    desc.Filter = magnify ? D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT :
                                            D3D12_FILTER_MIN_MAG_MIP_POINT;
                else if (minify == 1)
                    desc.Filter = magnify ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT :
                                            D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
                else
                    desc.Filter = magnify ? D3D12_FILTER_MIN_MAG_MIP_LINEAR :
                                            D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
                const D3D12_TEXTURE_ADDRESS_MODE mode {repeat ? D3D12_TEXTURE_ADDRESS_MODE_WRAP :
                                                                D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
                desc.AddressU = mode;
                desc.AddressV = mode;
                desc.AddressW = mode;
                desc.MaxAnisotropy = 1;
                desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                desc.MinLOD = 0.0f;
                desc.MaxLOD = D3D12_FLOAT32_MAX;
                mDevice->CreateSampler(&desc, handle);
                handle.ptr += mSamplerDescriptorSize;
            }
        }
    }
}

bool RendererD3D12::createRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE samplerRange {};
    samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors = 1;
    samplerRange.BaseShaderRegister = 0;
    samplerRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[3] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.RegisterSpace = 0;
    parameters[0].Constants.Num32BitValues = NUM_ROOT_CONSTANTS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &samplerRange;
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc {};
    desc.NumParameters = 3;
    desc.pParameters = parameters;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT result {
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)};
    if (FAILED(result)) {
        LOG(LogError) << "D3D12SerializeRootSignature() failed: "
                      << (error ? static_cast<const char*>(error->GetBufferPointer()) : "");
        return false;
    }
    result = mDevice->CreateRootSignature(0, signature->GetBufferPointer(),
                                          signature->GetBufferSize(),
                                          IID_PPV_ARGS(&mRootSignature));
    if (FAILED(result)) {
        LOG(LogError) << "CreateRootSignature() failed: " << hresultString(result);
        return false;
    }
    return true;
}

bool RendererD3D12::createContext()
{
    LOG(LogInfo) << "Application renderer: Direct3D 12";

    if (!createDevice() || !createSwapChain() || !createRenderTargets() ||
        !createRootSignature())
        return false;

    createSamplers();
    beginFrame();

    uint8_t data[4] {255, 255, 255, 255};
    mWhiteTexture = createTexture(0, TextureType::BGRA, false, false, false, true, 1, 1, data);

    unsigned int textureWidth {static_cast<unsigned int>(getScreenWidth())};
    unsigned int textureHeight {static_cast<unsigned int>(getScreenHeight())};
    if (getScreenRotation() == 90 || getScreenRotation() == 270)
        std::swap(textureWidth, textureHeight);

    mPostProcTexture1 = createTexture(0, TextureType::BGRA, false, true, false, false,
                                      textureWidth, textureHeight, nullptr);
    mPostProcTexture2 = createTexture(0, TextureType::BGRA, false, true, false, false,
                                      textureWidth, textureHeight, nullptr);

    // The post-processing textures are also render targets.
    for (unsigned int texture : {mPostProcTexture1, mPostProcTexture2}) {
        Texture& entry {mTextures[texture]};
        // Recreate the resource with the render target flag.
        D3D12_RESOURCE_DESC desc {entry.resource->GetDesc()};
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_HEAP_PROPERTIES defaultHeap {heapProperties(D3D12_HEAP_TYPE_DEFAULT)};
        D3D12_CLEAR_VALUE clearValue {};
        clearValue.Format = desc.Format;
        clearValue.Color[3] = 1.0f;
        ComPtr<ID3D12Resource> resource;
        if (FAILED(mDevice->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                IID_PPV_ARGS(&resource)))) {
            LOG(LogError) << "Couldn't create post-processing render target";
            return false;
        }
        mFrames[mFrameIndex].pendingRelease.emplace_back(entry.resource);
        entry.resource = resource;
        entry.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        entry.renderTarget = true;
        entry.rtvIndex = static_cast<int>(FRAME_COUNT + (texture == mPostProcTexture1 ? 0 : 1));

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle {mSRVHeap->GetCPUDescriptorHandleForHeapStart()};
        srvHandle.ptr += static_cast<SIZE_T>(texture) * mSRVDescriptorSize;
        mDevice->CreateShaderResourceView(entry.resource.Get(), &srvDesc, srvHandle);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
        rtvHandle.ptr += static_cast<SIZE_T>(entry.rtvIndex) * mRTVDescriptorSize;
        mDevice->CreateRenderTargetView(entry.resource.Get(), nullptr, rtvHandle);
    }

    LOG(LogInfo) << "Anti-aliasing: disabled";
    return true;
}

void RendererD3D12::destroyContext()
{
    if (!mDevice)
        return;

    flushGPU();

    for (auto& frame : mFrames) {
        processPendingReleases(frame);
        if (frame.uploadBuffer)
            frame.uploadBuffer->Unmap(0, nullptr);
        frame.uploadBuffer.Reset();
        frame.uploadData = nullptr;
        frame.allocator.Reset();
        frame.overflowBuffers.clear();
    }

    mTextures.clear();
    mFreeTextureSlots.clear();
    mPipelines.clear();
    mVertexShaders.clear();
    mPixelShaders.clear();
    for (auto& backBuffer : mBackBuffers)
        backBuffer.Reset();
    mRootSignature.Reset();
    mRTVHeap.Reset();
    mSRVHeap.Reset();
    mSamplerHeap.Reset();
    mCommandList.Reset();
    mSwapChain.Reset();
    mCommandQueue.Reset();
    mFence.Reset();
    mDevice.Reset();
    mFactory.Reset();

    if (mFenceEvent != nullptr) {
        CloseHandle(mFenceEvent);
        mFenceEvent = nullptr;
    }
    mFrameActive = false;
}

bool RendererD3D12::loadShaders()
{
    LOG(LogInfo) << "Loading shaders...";

    struct ShaderEntry {
        Program program;
        const char* vertexEntry;
        const char* pixelEntry;
    };

    const ShaderEntry entries[] {
        {Program::CORE, "VSCore", "PSCore"},
        {Program::BLUR_HORIZONTAL, "VSBlur", "PSBlurHorizontal"},
        {Program::BLUR_VERTICAL, "VSBlur", "PSBlurVertical"},
        {Program::SCANLINES, "VSScanlines", "PSScanlines"},
    };

    UINT flags {D3DCOMPILE_ENABLE_STRICTNESS};
#if !defined(NDEBUG)
    flags |= D3DCOMPILE_DEBUG;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    const size_t sourceLength {std::strlen(ShadersD3D12::sSource)};

    for (const auto& entry : entries) {
        for (int stage {0}; stage < 2; ++stage) {
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> errors;
            const HRESULT result {D3DCompile(ShadersD3D12::sSource, sourceLength, "es-de.hlsl",
                                             nullptr, nullptr,
                                             stage == 0 ? entry.vertexEntry : entry.pixelEntry,
                                             stage == 0 ? "vs_5_0" : "ps_5_0", flags, 0, &blob,
                                             &errors)};
            if (FAILED(result)) {
                LOG(LogError) << "Couldn't compile shader "
                              << (stage == 0 ? entry.vertexEntry : entry.pixelEntry) << ": "
                              << (errors ? static_cast<const char*>(errors->GetBufferPointer()) :
                                           hresultString(result));
                return false;
            }
            if (stage == 0)
                mVertexShaders[entry.program] = blob;
            else
                mPixelShaders[entry.program] = blob;
        }
    }

    return true;
}

ID3D12PipelineState* RendererD3D12::getPipeline(Program program,
                                                BlendFactor srcBlendFactor,
                                                BlendFactor dstBlendFactor,
                                                bool /*postProcessingTarget*/)
{
    // All render targets use the same format so the pipeline only depends on the program
    // and the blend factors.
    const unsigned long long key {(static_cast<unsigned long long>(program) << 16) |
                                  (static_cast<unsigned long long>(srcBlendFactor) << 8) |
                                  static_cast<unsigned long long>(dstBlendFactor)};

    auto it = mPipelines.find(key);
    if (it != mPipelines.end())
        return it->second.Get();

    if (mVertexShaders.find(program) == mVertexShaders.end())
        return nullptr;

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, position),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texcoord),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Vertex, color),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // OpenGL applies the same blend factors to the alpha channel, but Direct3D doesn't allow
    // color factors for alpha so these are replaced with the corresponding alpha factors.
    auto alphaFactor = [](D3D12_BLEND blend) {
        switch (blend) {
            case D3D12_BLEND_SRC_COLOR:
                return D3D12_BLEND_SRC_ALPHA;
            case D3D12_BLEND_INV_SRC_COLOR:
                return D3D12_BLEND_INV_SRC_ALPHA;
            case D3D12_BLEND_DEST_COLOR:
                return D3D12_BLEND_DEST_ALPHA;
            case D3D12_BLEND_INV_DEST_COLOR:
                return D3D12_BLEND_INV_DEST_ALPHA;
            default:
                return blend;
        }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
    desc.pRootSignature = mRootSignature.Get();
    desc.VS = {mVertexShaders[program]->GetBufferPointer(),
               mVertexShaders[program]->GetBufferSize()};
    desc.PS = {mPixelShaders[program]->GetBufferPointer(),
               mPixelShaders[program]->GetBufferSize()};
    desc.InputLayout = {inputLayout, 3};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = BACK_BUFFER_FORMAT;
    desc.SampleDesc.Count = 1;
    desc.SampleMask = UINT_MAX;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;

    D3D12_RENDER_TARGET_BLEND_DESC& blend {desc.BlendState.RenderTarget[0]};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = convertBlendFactor(srcBlendFactor);
    blend.DestBlend = convertBlendFactor(dstBlendFactor);
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = alphaFactor(convertBlendFactor(srcBlendFactor));
    blend.DestBlendAlpha = alphaFactor(convertBlendFactor(dstBlendFactor));
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT result {mDevice->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))};
    if (FAILED(result)) {
        LOG(LogError) << "CreateGraphicsPipelineState() failed: " << hresultString(result);
        return nullptr;
    }
    mPipelines[key] = pipeline;
    return pipeline.Get();
}

// -----------------------------------------------------------------------------------------
// Frame handling.
// -----------------------------------------------------------------------------------------

void RendererD3D12::waitForFrame(Frame& frame)
{
    if (frame.fenceValue != 0 && mFence->GetCompletedValue() < frame.fenceValue) {
        mFence->SetEventOnCompletion(frame.fenceValue, mFenceEvent);
        WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);
    }
}

void RendererD3D12::processPendingReleases(Frame& frame)
{
    frame.pendingRelease.clear();
    frame.overflowBuffers.clear();
    for (unsigned int slot : frame.pendingTextureFrees) {
        if (slot < mTextures.size()) {
            mTextures[slot] = Texture {};
            mFreeTextureSlots.push_back(slot);
        }
    }
    frame.pendingTextureFrees.clear();
    frame.uploadOffset = 0;
}

void RendererD3D12::flushGPU()
{
    if (!mCommandQueue || !mFence)
        return;
    const UINT64 value {mNextFenceValue++};
    mCommandQueue->Signal(mFence.Get(), value);
    if (mFence->GetCompletedValue() < value) {
        mFence->SetEventOnCompletion(value, mFenceEvent);
        WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);
    }
}

void RendererD3D12::transitionBackBuffer(D3D12_RESOURCE_STATES newState)
{
    if (mBackBufferStates[mFrameIndex] == newState)
        return;
    const D3D12_RESOURCE_BARRIER barrier {transitionBarrier(
        mBackBuffers[mFrameIndex].Get(), mBackBufferStates[mFrameIndex], newState)};
    mCommandList->ResourceBarrier(1, &barrier);
    mBackBufferStates[mFrameIndex] = newState;
}

void RendererD3D12::restoreFrameState()
{
    ID3D12DescriptorHeap* heaps[] {mSRVHeap.Get(), mSamplerHeap.Get()};
    mCommandList->SetDescriptorHeaps(2, heaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
    rtv.ptr += static_cast<SIZE_T>(mFrameIndex) * mRTVDescriptorSize;
    setRenderTarget(rtv, false);
    applyViewportAndScissor();
}

void RendererD3D12::beginFrame()
{
    Frame& frame {mFrames[mFrameIndex]};
    waitForFrame(frame);
    processPendingReleases(frame);

    frame.allocator->Reset();
    mCommandList->Reset(frame.allocator.Get(), nullptr);
    mFrameActive = true;

    transitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET);
    restoreFrameState();

    // Equivalent to the glClear() call after swapping buffers in the OpenGL renderer.
    const float clearColor[4] {0.0f, 0.0f, 0.0f, 1.0f};
    mCommandList->ClearRenderTargetView(mCurrentRTV, clearColor, 0, nullptr);
}

void RendererD3D12::endFrameAndPresent()
{
    Frame& frame {mFrames[mFrameIndex]};

    transitionBackBuffer(D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->Close();
    ID3D12CommandList* lists[] {mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1, lists);
    mFrameActive = false;

    const HRESULT result {mSwapChain->Present(mVSync ? 1 : 0, 0)};
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        if (!mDeviceLost) {
            LOG(LogError) << "Direct3D 12 device lost: "
                          << hresultString(mDevice->GetDeviceRemovedReason());
        }
        mDeviceLost = true;
    }

    frame.fenceValue = mNextFenceValue++;
    mCommandQueue->Signal(mFence.Get(), frame.fenceValue);
    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
}

void RendererD3D12::executeAndWait()
{
    // Submits the commands recorded so far and waits for them to complete, then reopens
    // the command list so that rendering of the current frame can continue.
    Frame& frame {mFrames[mFrameIndex]};
    mCommandList->Close();
    ID3D12CommandList* lists[] {mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1, lists);
    flushGPU();

    processPendingReleases(frame);
    frame.allocator->Reset();
    mCommandList->Reset(frame.allocator.Get(), nullptr);
    restoreFrameState();
}

void RendererD3D12::swapBuffers()
{
    if (!mFrameActive || mDeviceLost)
        return;
    endFrameAndPresent();
    beginFrame();
}

void RendererD3D12::setSwapInterval()
{
    mVSync = Settings::getInstance()->getBool("VSync");
    LOG(LogInfo) << "VSync: " << (mVSync ? "enabled" : "disabled");
}

// -----------------------------------------------------------------------------------------
// Upload heap.
// -----------------------------------------------------------------------------------------

unsigned char* RendererD3D12::allocateUpload(size_t size,
                                             size_t alignment,
                                             ID3D12Resource** buffer,
                                             UINT64* offset)
{
    Frame& frame {mFrames[mFrameIndex]};
    const size_t alignedOffset {alignUp(frame.uploadOffset, alignment)};

    if (alignedOffset + size <= frame.uploadSize) {
        frame.uploadOffset = alignedOffset + size;
        *buffer = frame.uploadBuffer.Get();
        *offset = alignedOffset;
        return frame.uploadData + alignedOffset;
    }

    // Doesn't fit, create a dedicated buffer which is released when the frame completes.
    const D3D12_HEAP_PROPERTIES uploadHeap {heapProperties(D3D12_HEAP_TYPE_UPLOAD)};
    const D3D12_RESOURCE_DESC desc {bufferDesc(alignUp(size, 65536))};
    ComPtr<ID3D12Resource> overflow;
    if (FAILED(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&overflow)))) {
        LOG(LogError) << "Couldn't allocate " << size << " bytes of upload memory";
        return nullptr;
    }
    unsigned char* data {nullptr};
    D3D12_RANGE readRange {0, 0};
    overflow->Map(0, &readRange, reinterpret_cast<void**>(&data));
    *buffer = overflow.Get();
    *offset = 0;
    frame.overflowBuffers.emplace_back(overflow);
    return data;
}

// -----------------------------------------------------------------------------------------
// Textures.
// -----------------------------------------------------------------------------------------

unsigned int RendererD3D12::allocateTextureSlot()
{
    if (!mFreeTextureSlots.empty()) {
        const unsigned int slot {mFreeTextureSlots.back()};
        mFreeTextureSlots.pop_back();
        return slot;
    }
    if (mTextures.size() >= MAX_TEXTURES) {
        LOG(LogError) << "RendererD3D12: Texture limit (" << MAX_TEXTURES << ") reached";
        return 0;
    }
    mTextures.emplace_back();
    return static_cast<unsigned int>(mTextures.size() - 1);
}

void RendererD3D12::transition(Texture& texture, D3D12_RESOURCE_STATES newState)
{
    if (texture.state == newState)
        return;
    const D3D12_RESOURCE_BARRIER barrier {
        transitionBarrier(texture.resource.Get(), texture.state, newState)};
    mCommandList->ResourceBarrier(1, &barrier);
    texture.state = newState;
}

void RendererD3D12::uploadTextureData(Texture& texture,
                                      unsigned int mipLevel,
                                      unsigned int x,
                                      unsigned int y,
                                      unsigned int width,
                                      unsigned int height,
                                      const unsigned char* data,
                                      unsigned int bpp)
{
    const size_t sourcePitch {static_cast<size_t>(width) * bpp};
    const size_t rowPitch {alignUp(sourcePitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)};

    ID3D12Resource* uploadBuffer {nullptr};
    UINT64 uploadOffset {0};
    unsigned char* destination {allocateUpload(rowPitch * height,
                                               D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                               &uploadBuffer, &uploadOffset)};
    if (destination == nullptr)
        return;

    for (unsigned int row {0}; row < height; ++row)
        std::memcpy(destination + row * rowPitch, data + row * sourcePitch, sourcePitch);

    D3D12_TEXTURE_COPY_LOCATION source {};
    source.pResource = uploadBuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = uploadOffset;
    source.PlacedFootprint.Footprint.Format = texture.format;
    source.PlacedFootprint.Footprint.Width = width;
    source.PlacedFootprint.Footprint.Height = height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    D3D12_TEXTURE_COPY_LOCATION target {};
    target.pResource = texture.resource.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = mipLevel;

    transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
    mCommandList->CopyTextureRegion(&target, x, y, 0, &source, nullptr);
}

unsigned int RendererD3D12::createTexture(const unsigned int /*texUnit*/,
                                          const TextureType type,
                                          const bool linearMinify,
                                          const bool linearMagnify,
                                          const bool mipmapping,
                                          const bool repeat,
                                          const unsigned int width,
                                          const unsigned int height,
                                          void* data)
{
    if (!mDevice || !mFrameActive || width == 0 || height == 0)
        return 0;

    const unsigned int slot {allocateTextureSlot()};
    if (slot == 0)
        return 0;

    Texture& texture {mTextures[slot]};
    texture = Texture {};
    texture.format = convertTextureType(type);
    texture.width = width;
    texture.height = height;
    texture.inUse = true;

    const bool generateMips {mipmapping && data != nullptr && type != TextureType::RED};
    if (generateMips) {
        unsigned int levels {1};
        unsigned int size {std::max(width, height)};
        while (size > 1) {
            size /= 2;
            ++levels;
        }
        texture.mipLevels = levels;
    }

    const unsigned int minify {mipmapping ? (generateMips ? 2u : 1u) : (linearMinify ? 1u : 0u)};
    texture.samplerIndex = ((minify * 2) + (linearMagnify ? 1 : 0)) * 2 + (repeat ? 1 : 0);

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(texture.mipLevels);
    desc.Format = texture.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES defaultHeap {heapProperties(D3D12_HEAP_TYPE_DEFAULT)};
    const HRESULT result {mDevice->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture.resource))};
    if (FAILED(result)) {
        LOG(LogError) << "RendererD3D12: Couldn't create " << width << "x" << height
                      << " texture: " << hresultString(result);
        texture = Texture {};
        mFreeTextureSlots.push_back(slot);
        return 0;
    }
    texture.state = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = texture.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = texture.mipLevels;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle {mSRVHeap->GetCPUDescriptorHandleForHeapStart()};
    srvHandle.ptr += static_cast<SIZE_T>(slot) * mSRVDescriptorSize;
    mDevice->CreateShaderResourceView(texture.resource.Get(), &srvDesc, srvHandle);

    if (data != nullptr) {
        const unsigned int bpp {bytesPerPixel(texture.format)};
        const unsigned char* level {static_cast<const unsigned char*>(data)};
        uploadTextureData(texture, 0, 0, 0, width, height, level, bpp);

        if (generateMips) {
            std::vector<unsigned char> current;
            unsigned int levelWidth {width};
            unsigned int levelHeight {height};
            for (unsigned int mip {1}; mip < texture.mipLevels; ++mip) {
                unsigned int newWidth {0};
                unsigned int newHeight {0};
                std::vector<unsigned char> next {
                    downsample(mip == 1 ? level : current.data(), levelWidth, levelHeight,
                               newWidth, newHeight)};
                uploadTextureData(texture, mip, 0, 0, newWidth, newHeight, next.data(), 4);
                current.swap(next);
                levelWidth = newWidth;
                levelHeight = newHeight;
            }
        }
    }

    transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return slot;
}

void RendererD3D12::destroyTexture(const unsigned int texture)
{
    if (!mDevice || texture == 0 || texture >= mTextures.size() || !mTextures[texture].inUse)
        return;

    // The GPU may still be using the texture, so release it when the current frame is done.
    Frame& frame {mFrames[mFrameIndex]};
    frame.pendingRelease.emplace_back(mTextures[texture].resource);
    frame.pendingTextureFrees.push_back(texture);
    mTextures[texture].inUse = false;
    if (mBoundTexture == texture)
        mBoundTexture = 0;
}

void RendererD3D12::updateTexture(const unsigned int texture,
                                  const unsigned int /*texUnit*/,
                                  const TextureType type,
                                  const unsigned int x,
                                  const unsigned int y,
                                  const unsigned int width,
                                  const unsigned int height,
                                  void* data)
{
    if (!mFrameActive || texture == 0 || texture >= mTextures.size() ||
        !mTextures[texture].inUse || data == nullptr || width == 0 || height == 0)
        return;

    Texture& entry {mTextures[texture]};
    const unsigned int bpp {bytesPerPixel(convertTextureType(type))};
    uploadTextureData(entry, 0, x, y, width, height, static_cast<const unsigned char*>(data), bpp);
    transition(entry, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void RendererD3D12::bindTexture(const unsigned int texture, const unsigned int texUnit)
{
    // Only texture unit 0 is used by the shaders.
    if (texUnit != 0)
        return;
    mBoundTexture = texture;
}

// -----------------------------------------------------------------------------------------
// Drawing.
// -----------------------------------------------------------------------------------------

void RendererD3D12::setMatrix(const glm::mat4& matrix)
{
    mTrans = getProjectionMatrix() * matrix;
}

void RendererD3D12::setRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool postProcessingTarget)
{
    mCurrentRTV = rtv;
    mRenderingToPostProcTarget = postProcessingTarget;
    mCommandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
}

void RendererD3D12::setViewport(const Rect& viewport)
{
    // ES-DE rectangles use a top-left origin, which is also what Direct3D uses.
    mD3DViewport.TopLeftX = static_cast<float>(viewport.x);
    mD3DViewport.TopLeftY = static_cast<float>(viewport.y);
    mD3DViewport.Width = static_cast<float>(viewport.w);
    mD3DViewport.Height = static_cast<float>(viewport.h);
    mD3DViewport.MinDepth = 0.0f;
    mD3DViewport.MaxDepth = 1.0f;
    if (mFrameActive)
        applyViewportAndScissor();
}

void RendererD3D12::setScissor(const Rect& scissor)
{
    mScissor = scissor;
    mScissorEnabled = !(scissor.x == 0 && scissor.y == 0 && scissor.w == 0 && scissor.h == 0);
    if (mFrameActive)
        applyViewportAndScissor();
}

void RendererD3D12::applyViewportAndScissor()
{
    mCommandList->RSSetViewports(1, &mD3DViewport);

    // The scissor test can't be disabled in Direct3D 12, so cover the whole target instead.
    if (mScissorEnabled && !mRenderingToPostProcTarget) {
        mD3DScissor.left = static_cast<LONG>(std::max(0, mScissor.x));
        mD3DScissor.top = static_cast<LONG>(std::max(0, mScissor.y));
        mD3DScissor.right =
            std::max(mD3DScissor.left, static_cast<LONG>(mScissor.x + mScissor.w));
        mD3DScissor.bottom =
            std::max(mD3DScissor.top, static_cast<LONG>(mScissor.y + mScissor.h));
    }
    else {
        mD3DScissor.left = 0;
        mD3DScissor.top = 0;
        mD3DScissor.right = D3D12_VIEWPORT_BOUNDS_MAX;
        mD3DScissor.bottom = D3D12_VIEWPORT_BOUNDS_MAX;
    }
    mCommandList->RSSetScissorRects(1, &mD3DScissor);
}

void RendererD3D12::draw(Program program,
                         const Vertex* vertices,
                         unsigned int numVertices,
                         const DrawConstants& constants,
                         BlendFactor srcBlendFactor,
                         BlendFactor dstBlendFactor)
{
    ID3D12PipelineState* pipeline {
        getPipeline(program, srcBlendFactor, dstBlendFactor, mRenderingToPostProcTarget)};
    if (pipeline == nullptr)
        return;

    ID3D12Resource* buffer {nullptr};
    UINT64 offset {0};
    const size_t size {sizeof(Vertex) * numVertices};
    unsigned char* destination {allocateUpload(size, 16, &buffer, &offset)};
    if (destination == nullptr)
        return;
    std::memcpy(destination, vertices, size);

    D3D12_VERTEX_BUFFER_VIEW view {};
    view.BufferLocation = buffer->GetGPUVirtualAddress() + offset;
    view.SizeInBytes = static_cast<UINT>(size);
    view.StrideInBytes = sizeof(Vertex);

    unsigned int texture {mBoundTexture};
    if (texture == 0 || texture >= mTextures.size() || !mTextures[texture].inUse)
        texture = mWhiteTexture;
    const Texture& entry {mTextures[texture]};

    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle {mSRVHeap->GetGPUDescriptorHandleForHeapStart()};
    srvHandle.ptr += static_cast<UINT64>(texture) * mSRVDescriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE samplerHandle {mSamplerHeap->GetGPUDescriptorHandleForHeapStart()};
    samplerHandle.ptr += static_cast<UINT64>(entry.samplerIndex) * mSamplerDescriptorSize;

    mCommandList->SetPipelineState(pipeline);
    mCommandList->SetGraphicsRoot32BitConstants(0, NUM_ROOT_CONSTANTS, &constants, 0);
    mCommandList->SetGraphicsRootDescriptorTable(1, srvHandle);
    mCommandList->SetGraphicsRootDescriptorTable(2, samplerHandle);
    mCommandList->IASetVertexBuffers(0, 1, &view);
    mCommandList->DrawInstanced(numVertices, 1, 0, 0);
}

void RendererD3D12::drawTriangleStrips(const Vertex* vertices,
                                       const unsigned int numVertices,
                                       const BlendFactor srcBlendFactor,
                                       const BlendFactor dstBlendFactor)
{
    if (!mFrameActive || mDeviceLost || numVertices < 3)
        return;

    float width {0.0f};
    float height {0.0f};
    if (numVertices >= 4) {
        width = vertices[3].position[0] - vertices[1].position[0];
        height = vertices[3].position[1] - vertices[2].position[1];
    }

    DrawConstants constants {};
    std::memcpy(constants.mvp, &mTrans[0][0], sizeof(constants.mvp));
    constants.clipRegion[0] = vertices->clipRegion.x;
    constants.clipRegion[1] = vertices->clipRegion.y;
    constants.clipRegion[2] = vertices->clipRegion.z;
    constants.clipRegion[3] = vertices->clipRegion.w;
    constants.brightness = vertices->brightness;
    constants.saturation = vertices->saturation;
    constants.opacity = vertices->opacity;
    constants.dimming = vertices->dimming;
    constants.cornerRadius = vertices->cornerRadius;
    constants.reflectionsFalloff = vertices->reflectionsFalloff;
    constants.shaderFlags = vertices->shaderFlags;

    if (vertices->shaders == 0 || vertices->shaders & Shader::CORE) {
        constants.texSize[0] = width;
        constants.texSize[1] = height;
        draw(Program::CORE, vertices, numVertices, constants, srcBlendFactor, dstBlendFactor);
    }
    else if (vertices->shaders & Shader::BLUR_HORIZONTAL) {
        constants.blurStrength =
            (vertices->blurStrength / getScreenWidth()) * getScreenResolutionModifier();
        draw(Program::BLUR_HORIZONTAL, vertices, numVertices, constants, srcBlendFactor,
             dstBlendFactor);
    }
    else if (vertices->shaders & Shader::BLUR_VERTICAL) {
        constants.blurStrength =
            (vertices->blurStrength / getScreenHeight()) * getScreenResolutionModifier();
        draw(Program::BLUR_VERTICAL, vertices, numVertices, constants, srcBlendFactor,
             dstBlendFactor);
    }
    else if (vertices->shaders & Shader::SCANLINES) {
        // Same scaling logic as in RendererOpenGL.
        const float shaderWidth {width * 1.2f};
        const float resolutionModifier {getScreenResolutionModifier()};
        const float relativeHeight {height / getScreenHeight()};
        float shaderHeight {0.0f};
        if (relativeHeight == 1.0f) {
            const float modifier {1.30f - (0.1f * resolutionModifier)};
            shaderHeight = height * modifier;
        }
        else {
            const float relativeWidth {width / getScreenWidth()};
            const float relativeAdjustment {(relativeWidth + relativeHeight) / 2.0f};
            const float modifier {1.41f + relativeAdjustment / 7.0f -
                                  (0.14f * resolutionModifier)};
            shaderHeight = height * modifier;
        }
        constants.texSize[0] = shaderWidth;
        constants.texSize[1] = shaderHeight;
        draw(Program::SCANLINES, vertices, numVertices, constants, srcBlendFactor,
             dstBlendFactor);
    }
}

void RendererD3D12::shaderPostprocessing(unsigned int shaders,
                                         const Renderer::postProcessingParams& parameters,
                                         unsigned char* textureRGBA)
{
    if (!mFrameActive || mDeviceLost || mPostProcTexture1 == 0 || mPostProcTexture2 == 0)
        return;

    const float widthf {getScreenWidth()};
    const float heightf {getScreenHeight()};
    const int screenRotation {getScreenRotation()};
    const bool rotated {screenRotation == 90 || screenRotation == 270};

    Texture& postTex1 {mTextures[mPostProcTexture1]};
    const unsigned int textureWidth {postTex1.width};
    const unsigned int textureHeight {postTex1.height};

    // Full screen quad. Compared to the OpenGL renderer the texture coordinates are flipped
    // vertically, as the first row of a Direct3D render target is the top of the screen.
    Vertex vertices[4];
    // clang-format off
    vertices[0] = {{0.0f,   0.0f   }, {0.0f, 0.0f}, 0xFFFFFFFF};
    vertices[1] = {{0.0f,   heightf}, {0.0f, 1.0f}, 0xFFFFFFFF};
    vertices[2] = {{widthf, 0.0f   }, {1.0f, 0.0f}, 0xFFFFFFFF};
    vertices[3] = {{widthf, heightf}, {1.0f, 1.0f}, 0xFFFFFFFF};
    // clang-format on

    vertices->opacity = parameters.opacity;
    vertices->saturation = parameters.saturation;
    vertices->dimming = parameters.dimming;
    vertices->blurStrength = parameters.blurStrength;
    vertices->shaderFlags = ShaderFlags::POST_PROCESSING | ShaderFlags::PREMULTIPLIED;
    if (rotated)
        vertices->shaderFlags |= ShaderFlags::ROTATED;

    // Build the list of passes.
    std::vector<unsigned int> passes;
    if (shaders & Shader::CORE)
        passes.push_back(Shader::CORE);
    if (shaders & Shader::BLUR_HORIZONTAL) {
        for (unsigned int i {0}; i < std::max(1u, parameters.blurPasses); ++i)
            passes.push_back(Shader::BLUR_HORIZONTAL);
    }
    if (shaders & Shader::BLUR_VERTICAL) {
        for (unsigned int i {0}; i < std::max(1u, parameters.blurPasses); ++i)
            passes.push_back(Shader::BLUR_VERTICAL);
    }
    if (shaders & Shader::SCANLINES)
        passes.push_back(Shader::SCANLINES);
    if (passes.empty())
        return;

    const glm::mat4 previousTrans {mTrans};
    setMatrix(getIdentity());
    if (rotated && !textureRGBA)
        mTrans = getProjectionMatrixNormal() * getIdentity();

    // Copy the screen contents to the first post-processing texture.
    const D3D12_VIEWPORT savedViewport {mD3DViewport};
    {
        transitionBackBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(postTex1, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = mBackBuffers[mFrameIndex].Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION target {};
        target.pResource = postTex1.resource.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        target.SubresourceIndex = 0;

        D3D12_BOX box {};
        box.left = static_cast<UINT>(std::max(0, mViewport.x));
        box.top = static_cast<UINT>(std::max(0, mViewport.y));
        box.right = std::min(box.left + textureWidth, static_cast<UINT>(mWindowWidth));
        box.bottom = std::min(box.top + textureHeight, static_cast<UINT>(mWindowHeight));
        box.front = 0;
        box.back = 1;
        mCommandList->CopyTextureRegion(&target, 0, 0, 0, &source, &box);

        transitionBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(postTex1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    unsigned int sourceTexture {mPostProcTexture1};
    unsigned int targetTexture {mPostProcTexture2};
    const unsigned int previousBoundTexture {mBoundTexture};

    D3D12_VIEWPORT textureViewport {};
    textureViewport.Width = static_cast<float>(textureWidth);
    textureViewport.Height = static_cast<float>(textureHeight);
    textureViewport.MaxDepth = 1.0f;

    for (size_t i {0}; i < passes.size(); ++i) {
        vertices->shaders = passes[i];
        const bool lastPass {i == passes.size() - 1};
        bindTexture(sourceTexture, 0);

        if (lastPass && !textureRGBA) {
            // The last pass renders directly to the screen.
            D3D12_CPU_DESCRIPTOR_HANDLE rtv {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
            rtv.ptr += static_cast<SIZE_T>(mFrameIndex) * mRTVDescriptorSize;
            setRenderTarget(rtv, false);
            mD3DViewport = savedViewport;
            applyViewportAndScissor();
            drawTriangleStrips(vertices, 4, BlendFactor::SRC_ALPHA,
                               BlendFactor::ONE_MINUS_SRC_ALPHA);
            break;
        }

        Texture& target {mTextures[targetTexture]};
        transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
        rtv.ptr += static_cast<SIZE_T>(target.rtvIndex) * mRTVDescriptorSize;
        setRenderTarget(rtv, true);
        mD3DViewport = textureViewport;
        applyViewportAndScissor();
        const float clearColor[4] {0.0f, 0.0f, 0.0f, 1.0f};
        mCommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        drawTriangleStrips(vertices, 4, BlendFactor::SRC_ALPHA, BlendFactor::ONE_MINUS_SRC_ALPHA);

        transition(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        std::swap(sourceTexture, targetTexture);
    }

    if (textureRGBA) {
        // The result is in sourceTexture after the last swap. Copy it to a readback buffer,
        // wait for the GPU and then copy the rows in reverse order, as ES-DE expects the
        // OpenGL convention of the first row being the bottom of the screen. The pixel
        // format is BGRA, the same as the desktop OpenGL renderer returns.
        Texture& result {mTextures[sourceTexture]};
        const size_t rowPitch {
            alignUp(static_cast<size_t>(textureWidth) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)};
        const size_t readbackSize {rowPitch * textureHeight};

        const D3D12_HEAP_PROPERTIES readbackHeap {heapProperties(D3D12_HEAP_TYPE_READBACK)};
        const D3D12_RESOURCE_DESC desc {bufferDesc(readbackSize)};
        ComPtr<ID3D12Resource> readback;
        if (SUCCEEDED(mDevice->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE,
                                                       &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, IID_PPV_ARGS(&readback)))) {
            transition(result, D3D12_RESOURCE_STATE_COPY_SOURCE);

            D3D12_TEXTURE_COPY_LOCATION source {};
            source.pResource = result.resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION target {};
            target.pResource = readback.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint.Offset = 0;
            target.PlacedFootprint.Footprint.Format = result.format;
            target.PlacedFootprint.Footprint.Width = textureWidth;
            target.PlacedFootprint.Footprint.Height = textureHeight;
            target.PlacedFootprint.Footprint.Depth = 1;
            target.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

            mCommandList->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            transition(result, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            executeAndWait();

            unsigned char* mapped {nullptr};
            D3D12_RANGE readRange {0, readbackSize};
            if (SUCCEEDED(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)))) {
                const size_t rowSize {static_cast<size_t>(textureWidth) * 4};
                for (unsigned int row {0}; row < textureHeight; ++row) {
                    std::memcpy(textureRGBA + row * rowSize,
                                mapped + (textureHeight - 1 - row) * rowPitch, rowSize);
                }
                D3D12_RANGE writeRange {0, 0};
                readback->Unmap(0, &writeRange);
            }
        }
        else {
            LOG(LogError) << "RendererD3D12: Couldn't create readback buffer";
        }
    }

    // Restore the screen as the render target.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv {mRTVHeap->GetCPUDescriptorHandleForHeapStart()};
    rtv.ptr += static_cast<SIZE_T>(mFrameIndex) * mRTVDescriptorSize;
    setRenderTarget(rtv, false);
    mD3DViewport = savedViewport;
    applyViewportAndScissor();
    mBoundTexture = previousBoundTexture;
    mTrans = previousTrans;
}

#endif // USE_D3D12_RENDERER
