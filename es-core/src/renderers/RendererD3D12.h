//  SPDX-License-Identifier: MIT
//
//  ES-DE Frontend
//  RendererD3D12.h
//
//  Direct3D 12 renderer, used by the UWP (Xbox Dev Mode) build where OpenGL is not
//  available. It implements the same interface and visual output as RendererOpenGL.
//
//  The swap chain is created for the SDL window, either a CoreWindow (UWP) or an HWND
//  (Win32, which makes it possible to test the renderer on a Windows desktop).
//

#ifndef ES_CORE_RENDERER_RENDERER_D3D12_H
#define ES_CORE_RENDERER_RENDERER_D3D12_H

#if defined(USE_D3D12_RENDERER)

#include "renderers/Renderer.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <map>
#include <memory>
#include <vector>

class RendererD3D12 : public Renderer
{
public:
    static RendererD3D12* getInstance();

    bool loadShaders() override;

    void setup() override;
    bool createContext() override;
    void destroyContext() override;

    void setMatrix(const glm::mat4& matrix) override;
    void setViewport(const Rect& viewport) override;
    void setScissor(const Rect& scissor) override;
    void setSwapInterval() override;
    void swapBuffers() override;

    unsigned int createTexture(const unsigned int texUnit,
                               const TextureType type,
                               const bool linearMinify,
                               const bool linearMagnify,
                               const bool mipmapping,
                               const bool repeat,
                               const unsigned int width,
                               const unsigned int height,
                               void* data) override;
    void destroyTexture(const unsigned int texture) override;
    void updateTexture(const unsigned int texture,
                       const unsigned int texUnit,
                       const TextureType type,
                       const unsigned int x,
                       const unsigned int y,
                       const unsigned int width,
                       const unsigned int height,
                       void* data) override;
    void bindTexture(const unsigned int texture, const unsigned int texUnit) override;
    void drawTriangleStrips(
        const Vertex* vertices,
        const unsigned int numVertices,
        const BlendFactor srcBlendFactor = BlendFactor::ONE,
        const BlendFactor dstBlendFactor = BlendFactor::ONE_MINUS_SRC_ALPHA) override;
    void shaderPostprocessing(
        const unsigned int shaders,
        const Renderer::postProcessingParams& parameters = postProcessingParams(),
        unsigned char* textureRGBA = nullptr) override;

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static constexpr unsigned int FRAME_COUNT {2};
    static constexpr unsigned int MAX_TEXTURES {8192};
    static constexpr unsigned int SAMPLER_COUNT {12};
    static constexpr unsigned int NUM_ROOT_CONSTANTS {32};

    // Must match the DrawConstants cbuffer in ShadersD3D12.h.
    struct DrawConstants {
        float mvp[16];
        float texSize[2];
        float padding0[2];
        float clipRegion[4];
        float brightness;
        float saturation;
        float opacity;
        float dimming;
        float cornerRadius;
        float reflectionsFalloff;
        float blurStrength;
        unsigned int shaderFlags;
    };
    static_assert(sizeof(DrawConstants) == NUM_ROOT_CONSTANTS * 4, "DrawConstants size mismatch");

    enum class Program {
        CORE,
        BLUR_HORIZONTAL,
        BLUR_VERTICAL,
        SCANLINES
    };

    struct Texture {
        ComPtr<ID3D12Resource> resource;
        DXGI_FORMAT format {DXGI_FORMAT_UNKNOWN};
        unsigned int width {0};
        unsigned int height {0};
        unsigned int mipLevels {1};
        unsigned int samplerIndex {0};
        D3D12_RESOURCE_STATES state {D3D12_RESOURCE_STATE_COMMON};
        bool renderTarget {false};
        int rtvIndex {-1};
        bool inUse {false};
    };

    // Per-frame resources.
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12Resource> uploadBuffer;
        unsigned char* uploadData {nullptr};
        size_t uploadSize {0};
        size_t uploadOffset {0};
        // Extra upload buffers created when the main one was full, released with the frame.
        std::vector<ComPtr<ID3D12Resource>> overflowBuffers;
        std::vector<ComPtr<ID3D12Resource>> pendingRelease;
        std::vector<unsigned int> pendingTextureFrees;
        UINT64 fenceValue {0};
    };

    RendererD3D12() noexcept;

    bool createDevice();
    bool createSwapChain();
    bool createRenderTargets();
    bool createRootSignature();
    ID3D12PipelineState* getPipeline(Program program,
                                     BlendFactor srcBlendFactor,
                                     BlendFactor dstBlendFactor,
                                     bool postProcessingTarget);
    void createSamplers();

    void beginFrame();
    void endFrameAndPresent();
    void flushGPU();
    void waitForFrame(Frame& frame);
    void executeAndWait();
    void restoreFrameState();
    void processPendingReleases(Frame& frame);

    // Allocates space in the per-frame upload heap and returns the CPU pointer.
    unsigned char* allocateUpload(size_t size,
                                  size_t alignment,
                                  ID3D12Resource** buffer,
                                  UINT64* offset);

    unsigned int allocateTextureSlot();
    void transition(Texture& texture, D3D12_RESOURCE_STATES newState);
    void transitionBackBuffer(D3D12_RESOURCE_STATES newState);
    void uploadTextureData(Texture& texture,
                           unsigned int mipLevel,
                           unsigned int x,
                           unsigned int y,
                           unsigned int width,
                           unsigned int height,
                           const unsigned char* data,
                           unsigned int bytesPerPixel);

    void setRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool postProcessingTarget);
    void applyViewportAndScissor();
    void draw(Program program,
              const Vertex* vertices,
              unsigned int numVertices,
              const DrawConstants& constants,
              BlendFactor srcBlendFactor,
              BlendFactor dstBlendFactor);

    static DXGI_FORMAT convertTextureType(const TextureType type);
    static D3D12_BLEND convertBlendFactor(const BlendFactor blendFactor);

    ComPtr<IDXGIFactory4> mFactory;
    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;
    ComPtr<ID3D12Fence> mFence;
    HANDLE mFenceEvent;
    UINT64 mNextFenceValue;

    ComPtr<ID3D12DescriptorHeap> mRTVHeap;
    ComPtr<ID3D12DescriptorHeap> mSRVHeap;
    ComPtr<ID3D12DescriptorHeap> mSamplerHeap;
    unsigned int mRTVDescriptorSize;
    unsigned int mSRVDescriptorSize;
    unsigned int mSamplerDescriptorSize;

    std::array<ComPtr<ID3D12Resource>, FRAME_COUNT> mBackBuffers;
    std::array<D3D12_RESOURCE_STATES, FRAME_COUNT> mBackBufferStates;
    std::array<Frame, FRAME_COUNT> mFrames;
    unsigned int mFrameIndex;
    bool mFrameActive;

    ComPtr<ID3D12RootSignature> mRootSignature;
    std::map<Program, ComPtr<ID3DBlob>> mVertexShaders;
    std::map<Program, ComPtr<ID3DBlob>> mPixelShaders;
    std::map<unsigned long long, ComPtr<ID3D12PipelineState>> mPipelines;

    std::vector<Texture> mTextures;
    std::vector<unsigned int> mFreeTextureSlots;
    unsigned int mBoundTexture;
    unsigned int mWhiteTexture;
    unsigned int mPostProcTexture1;
    unsigned int mPostProcTexture2;

    D3D12_CPU_DESCRIPTOR_HANDLE mCurrentRTV;
    bool mRenderingToPostProcTarget;
    D3D12_VIEWPORT mD3DViewport;
    D3D12_RECT mD3DScissor;
    bool mScissorEnabled;
    Rect mScissor;
    bool mVSync;
    bool mTearingSupported;
    bool mDeviceLost;

    friend Renderer;
};

#endif // USE_D3D12_RENDERER

#endif // ES_CORE_RENDERER_RENDERER_D3D12_H
