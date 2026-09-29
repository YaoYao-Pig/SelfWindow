#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <string>
#include <vector>
#include "geometry.h"
#include "warp.h"

namespace sw {
struct Monitor {
    HMONITOR handle{};
    RECT bounds{};
    std::wstring device, name, key;
    DWORD rotation{};
    bool primary{};
    int width() const { return bounds.right-bounds.left; }
    int height() const { return bounds.bottom-bounds.top; }
};
std::vector<Monitor> EnumerateMonitors();
struct Pixels { UINT width{},height{}; std::vector<unsigned char> bgra; };
void SaveBitmap(const Pixels& pixels,const std::wstring& path);

class Graphics {
public:
    Graphics();
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice runtimeDevice{nullptr};
    Pixels readTexture(ID3D11Texture2D* texture);
};

class Capture {
public:
    explicit Capture(Graphics& g):graphics(g){}
    ~Capture() { stop(); }
    void start(const Monitor& monitor);
    bool update();
    void stop() noexcept;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    int width{},height{};
    unsigned frames{};
private:
    Graphics& graphics;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
};

class Renderer {
public:
    Renderer(Graphics& g,HWND window,int w,int h);
    void draw(ID3D11ShaderResourceView* source,const Homography& transform,bool grid);
    void draw(ID3D11ShaderResourceView* source,const WarpMapping& mapping,bool grid);
    Pixels snapshot();
private:
    Graphics& graphics;
    int width,height;
    winrt::com_ptr<IDXGISwapChain1> swapchain;
    winrt::com_ptr<ID3D11Texture2D> targetTexture;
    winrt::com_ptr<ID3D11RenderTargetView> target;
    winrt::com_ptr<ID3D11VertexShader> vs;
    winrt::com_ptr<ID3D11PixelShader> ps;
    winrt::com_ptr<ID3D11Buffer> constants;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    winrt::com_ptr<ID3D11VertexShader> meshVs;
    winrt::com_ptr<ID3D11InputLayout> meshLayout;
    winrt::com_ptr<ID3D11Buffer> meshVertices,meshIndices;
    std::uint64_t meshRevision=0;
    UINT meshIndexCount=0;
    void drawFrame(ID3D11ShaderResourceView* source,const Homography& transform,bool grid,bool mesh);
};
}
