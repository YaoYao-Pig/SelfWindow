#include "graphics.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace sw {
std::vector<Monitor> EnumerateMonitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR h,HDC,LPRECT,LPARAM data)->BOOL {
        auto& out=*reinterpret_cast<std::vector<Monitor>*>(data);
        MONITORINFOEXW mi{}; mi.cbSize=sizeof(mi);
        if(!GetMonitorInfoW(h,&mi)) return TRUE;
        Monitor m; m.handle=h;m.bounds=mi.rcMonitor;m.device=mi.szDevice;m.primary=(mi.dwFlags&MONITORINFOF_PRIMARY)!=0;
        DISPLAY_DEVICEW dd{};dd.cb=sizeof(dd);
        if(EnumDisplayDevicesW(mi.szDevice,0,&dd,EDD_GET_DEVICE_INTERFACE_NAME)) {m.name=dd.DeviceString;m.key=dd.DeviceID;}
        if(m.key.empty()) m.key=m.device;
        DEVMODEW dm{};dm.dmSize=sizeof(dm);
        if(EnumDisplaySettingsW(mi.szDevice,ENUM_CURRENT_SETTINGS,&dm)) m.rotation=dm.dmDisplayOrientation;
        m.key+=L"|rotation="+std::to_wstring(m.rotation);
        out.push_back(m); return TRUE;
    },reinterpret_cast<LPARAM>(&monitors));
    std::sort(monitors.begin(),monitors.end(),[](const Monitor& a,const Monitor& b){return a.bounds.left==b.bounds.left?a.bounds.top<b.bounds.top:a.bounds.left<b.bounds.left;});
    return monitors;
}
Graphics::Graphics() {
    check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.put(),nullptr,context.put()));
    auto dxgi=device.as<IDXGIDevice>();
    com_ptr<IInspectable> rt;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),rt.put()));
    runtimeDevice=rt.as<IDirect3DDevice>();
}
void Capture::start(const Monitor& monitor) {
    stop();
    if(!GraphicsCaptureSession::IsSupported()) throw hresult_error(E_NOTIMPL,L"Windows Graphics Capture 不可用。");
    auto interop=get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
    check_hresult(interop->CreateForMonitor(monitor.handle,guid_of<GraphicsCaptureItem>(),put_abi(item)));
    auto size=item.Size();width=size.Width;height=size.Height;
    if(width!=monitor.width()||height!=monitor.height()) throw hresult_error(E_FAIL,L"捕获尺寸与显示器实际像素不符，请刷新显示器后重试。");
    pool=Direct3D11CaptureFramePool::CreateFreeThreaded(graphics.runtimeDevice,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
    session=pool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(false);
    session.StartCapture();
}
bool Capture::update() {
    if(!pool) return false;
    auto frame=pool.TryGetNextFrame();
    if(!frame) return false;
    const auto size=frame.ContentSize();
    if(size.Width!=width||size.Height!=height) throw hresult_error(E_FAIL,L"显示器方向或分辨率已改变，请重新开启校正。");
    auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> input;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),input.put_void()));
    if(!texture) {
        D3D11_TEXTURE2D_DESC d{};input->GetDesc(&d);d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=0;d.MiscFlags=0;
        check_hresult(graphics.device->CreateTexture2D(&d,nullptr,texture.put()));
        check_hresult(graphics.device->CreateShaderResourceView(texture.get(),nullptr,view.put()));
    }
    graphics.context->CopyResource(texture.get(),input.get());
    frame.Close();++frames;return true;
}
void Capture::stop() noexcept {
    try {if(session) session.Close();}catch(...){}
    try {if(pool) pool.Close();}catch(...){}
    session=nullptr;pool=nullptr;item=nullptr;view=nullptr;texture=nullptr;frames=0;
}
static const char* shader=R"HLSL(
cbuffer Params:register(b0) { float4 r0;float4 r1;float4 r2;float4 options; };
Texture2D source:register(t0);SamplerState linearClamp:register(s0);
struct V {float4 position:SV_POSITION;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID) {
    V v;float2 p=float2((id<<1)&2,id&2);v.uv=p;v.position=float4(p.x*2-1,1-p.y*2,0,1);return v;
}
V MeshVS(float2 p:POSITION,float2 uv:TEXCOORD0){V v;v.position=float4(p.x*2-1,1-p.y*2,0,1);v.uv=uv;return v;}
float4 PS(V v):SV_TARGET {
    float3 p=float3(v.uv,1);float z=dot(r2.xyz,p);
    if(abs(z)<0.000001) return float4(0,0,0,1);
    float2 uv=options.w>0.5?v.uv:float2(dot(r0.xyz,p),dot(r1.xyz,p))/z;
    if(any(uv<0)||any(uv>1)) return float4(0,0,0,1);
    if(options.x>0.5) {
        float2 px=uv*options.yz;
        float2 cell=abs(frac(px/80+0.5)-0.5)*80;
        float3 color=float3(0.055,0.075,0.105);
        if(min(cell.x,cell.y)<1.0) color=float3(0.28,0.45,0.55);
        if(min(min(px.x,px.y),min(options.y-px.x,options.z-px.y))<4) color=float3(0.25,0.9,0.7);
        if(abs(uv.x-0.5)*options.y<1.5||abs(uv.y-0.5)*options.z<1.5) color=float3(1,0.7,0.25);
        // A physical-pixel circle makes unintended portrait stretching visible.
        if(abs(length(px-options.yz/2)-min(options.y,options.z)*0.23)<2) color=float3(0.3,0.85,1);
        if(uv.x<0.12&&uv.y<0.08) color=float3(1,0.25,0.22);
        if(uv.x>0.88&&uv.y<0.08) color=float3(0.2,0.8,0.35);
        if(uv.x>0.88&&uv.y>0.92) color=float3(0.25,0.45,1);
        if(uv.x<0.12&&uv.y>0.92) color=float3(1,0.7,0.2);
        return float4(color,1);
    }
    return float4(source.Sample(linearClamp,uv).rgb,1);
})HLSL";
Renderer::Renderer(Graphics& g,HWND window,int w,int h):graphics(g),width(w),height(h) {
    auto dxgi=g.device.as<IDXGIDevice>();com_ptr<IDXGIAdapter> adapter;check_hresult(dxgi->GetAdapter(adapter.put()));
    com_ptr<IDXGIFactory2> factory;check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
    desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.Scaling=DXGI_SCALING_STRETCH;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    check_hresult(factory->CreateSwapChainForHwnd(g.device.get(),window,&desc,nullptr,nullptr,swapchain.put()));
    factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    check_hresult(swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),targetTexture.put_void()));
    check_hresult(g.device->CreateRenderTargetView(targetTexture.get(),nullptr,target.put()));
    com_ptr<ID3DBlob> vsBlob,psBlob,errors;
    auto compile=[&](const char* entry,const char* profile,com_ptr<ID3DBlob>& blob) {
        HRESULT hr=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob.put(),errors.put());
        if(FAILED(hr)) throw hresult_error(hr,errors?to_hstring(std::string(static_cast<char*>(errors->GetBufferPointer()),errors->GetBufferSize())):L"Shader compile failed");
        errors=nullptr;
    };
    compile("VS","vs_5_0",vsBlob);compile("PS","ps_5_0",psBlob);
    check_hresult(g.device->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,vs.put()));
    check_hresult(g.device->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,ps.put()));
    com_ptr<ID3DBlob> meshBlob;compile("MeshVS","vs_5_0",meshBlob);
    check_hresult(g.device->CreateVertexShader(meshBlob->GetBufferPointer(),meshBlob->GetBufferSize(),nullptr,meshVs.put()));
    D3D11_INPUT_ELEMENT_DESC layout[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
    check_hresult(g.device->CreateInputLayout(layout,2,meshBlob->GetBufferPointer(),meshBlob->GetBufferSize(),meshLayout.put()));
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=64;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check_hresult(g.device->CreateBuffer(&cb,nullptr,constants.put()));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    check_hresult(g.device->CreateSamplerState(&sd,sampler.put()));
}
void Renderer::draw(ID3D11ShaderResourceView* source,const Homography& transform,bool grid) {
    drawFrame(source,transform,grid,false);
}
void Renderer::draw(ID3D11ShaderResourceView* source,const WarpMapping& mapping,bool grid){
    if(mapping.projective){draw(source,mapping.homography,grid);return;}
    if(meshRevision!=mapping.revision){
        meshVertices=nullptr;meshIndices=nullptr;
        D3D11_BUFFER_DESC desc{};desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.ByteWidth=static_cast<UINT>(mapping.vertices.size()*sizeof(WarpVertex));
        D3D11_SUBRESOURCE_DATA data{};data.pSysMem=mapping.vertices.data();check_hresult(graphics.device->CreateBuffer(&desc,&data,meshVertices.put()));
        desc.BindFlags=D3D11_BIND_INDEX_BUFFER;desc.ByteWidth=static_cast<UINT>(mapping.indices.size()*sizeof(std::uint32_t));data.pSysMem=mapping.indices.data();
        check_hresult(graphics.device->CreateBuffer(&desc,&data,meshIndices.put()));meshIndexCount=static_cast<UINT>(mapping.indices.size());meshRevision=mapping.revision;
    }
    drawFrame(source,Homography{},grid,true);
}
void Renderer::drawFrame(ID3D11ShaderResourceView* source,const Homography& transform,bool grid,bool mesh) {
    auto inv=transform.inverse();
    float params[16]{};for(int r=0;r<3;++r)for(int c=0;c<3;++c) params[r*4+c]=static_cast<float>(inv.m[r*3+c]);
    params[12]=grid?1.f:0.f;params[13]=static_cast<float>(width);params[14]=static_cast<float>(height);
    params[15]=mesh?1.f:0.f;
    auto ctx=graphics.context.get();ctx->UpdateSubresource(constants.get(),0,nullptr,params,0,0);
    auto rt=target.get();ctx->OMSetRenderTargets(1,&rt,nullptr);
    const float black[]={0,0,0,1};ctx->ClearRenderTargetView(rt,black);
    D3D11_VIEWPORT vp{0,0,static_cast<float>(width),static_cast<float>(height),0,1};ctx->RSSetViewports(1,&vp);
    ctx->IASetInputLayout(mesh?meshLayout.get():nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(mesh?meshVs.get():vs.get(),nullptr,0);ctx->PSSetShader(ps.get(),nullptr,0);
    auto cb=constants.get();ctx->PSSetConstantBuffers(0,1,&cb);auto ss=sampler.get();ctx->PSSetSamplers(0,1,&ss);
    ctx->PSSetShaderResources(0,1,&source);
    if(mesh){UINT stride=sizeof(WarpVertex),offset=0;auto buffer=meshVertices.get();ctx->IASetVertexBuffers(0,1,&buffer,&stride,&offset);ctx->IASetIndexBuffer(meshIndices.get(),DXGI_FORMAT_R32_UINT,0);ctx->DrawIndexed(meshIndexCount,0,0);}
    else ctx->Draw(3,0);
    ID3D11ShaderResourceView* empty=nullptr;ctx->PSSetShaderResources(0,1,&empty);
    check_hresult(swapchain->Present(0,0));
}
Pixels Graphics::readTexture(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    com_ptr<ID3D11Texture2D> staging;check_hresult(device->CreateTexture2D(&d,nullptr,staging.put()));context->CopyResource(staging.get(),texture);
    D3D11_MAPPED_SUBRESOURCE map{};check_hresult(context->Map(staging.get(),0,D3D11_MAP_READ,0,&map));
    Pixels p{d.Width,d.Height,std::vector<unsigned char>(static_cast<size_t>(d.Width)*d.Height*4)};
    for(UINT y=0;y<d.Height;++y) memcpy(p.bgra.data()+static_cast<size_t>(y)*d.Width*4,static_cast<unsigned char*>(map.pData)+static_cast<size_t>(y)*map.RowPitch,d.Width*4);
    context->Unmap(staging.get(),0);return p;
}
Pixels Renderer::snapshot() {return graphics.readTexture(targetTexture.get());}
void SaveBitmap(const Pixels& p,const std::wstring& path) {
    BITMAPFILEHEADER fh{};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(BITMAPINFOHEADER);fh.bfSize=fh.bfOffBits+static_cast<DWORD>(p.bgra.size());
    BITMAPINFOHEADER ih{};ih.biSize=sizeof(ih);ih.biWidth=p.width;ih.biHeight=-static_cast<LONG>(p.height);ih.biPlanes=1;ih.biBitCount=32;ih.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&fh),sizeof(fh));out.write(reinterpret_cast<char*>(&ih),sizeof(ih));out.write(reinterpret_cast<const char*>(p.bgra.data()),p.bgra.size());
    if(!out) throw std::runtime_error("Could not save diagnostic bitmap");
}
}
