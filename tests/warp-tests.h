#pragma once
#include "../src/warp.h"
#include <ostream>
#include <chrono>

inline void RunWarpTests(std::ostream& log){
    using namespace sw;int assertions=0;
    auto require=[&](bool condition,const char* reason){++assertions;if(!condition)throw std::runtime_error(reason);};
    auto begin=std::chrono::steady_clock::now();
    WarpProfile legacy;legacy.corners={{{.10,.04},{.94,.15},{.91,.93},{.02,.89}}};WarpMapping old(legacy,2160,3840);
    require(old.projective,"Legacy calibration must retain exact homography path");
    for(int y=0;y<=12;++y)for(int x=0;x<=12;++x){Point uv{x/12.,y/12.};auto a=old.map(uv),b=FromQuad(legacy.corners).map(uv);require(std::hypot(a.x-b.x,a.y-b.y)<1e-12,"Legacy behavior changed");}
    for(bool portrait:{false,true})for(double yaw:{-55.,0.,32.,55.})for(int model=1;model<=2;++model)for(bool horizontal:{false,true}){
        WarpProfile p;p.mode=static_cast<WarpMode>(model);p.yaw=yaw;p.pitch=11;p.eyeX=-8;p.eyeY=6;
        p.widthCm=portrait?30:53.333333;p.heightCm=portrait?53.333333:30;p.horizontalAxis=horizontal;p.curvature=82;
        int width=portrait?2160:3840,height=portrait?3840:2160;
        WarpMapping m(p,width,height);
        require(m.fit>0&&m.fit<=1,"Invalid complete-frame fit");
        for(int j=0;j<=13;++j)for(int i=0;i<=13;++i){Point uv{i/13.,j/13.};auto a=m.map(uv),b=m.analytic(uv);
            require(a.x>=-1e-7&&a.x<=1+1e-7&&a.y>=-1e-7&&a.y<=1+1e-7,"Warped content outside monitor");
            require(std::hypot((a.x-b.x)*width,(a.y-b.y)*height)<.4,"Mesh approximation exceeds subpixel tolerance");}
        if(model==1){Quad q{};auto unit=IdentityQuad();for(int i=0;i<4;++i)q[i]=m.analytic(unit[i]);auto h=FromQuad(q);
            for(int i=0;i<=20;++i){Point uv{i/20.,.37};auto a=m.analytic(uv),b=h.map(uv);require(std::hypot(a.x-b.x,a.y-b.y)<1e-9,"Ray-plane model is not equivalent to homography");}}
    }
    WarpProfile plane;plane.mode=WarpMode::ViewPlane;plane.yaw=32;WarpMapping reference(plane,2160,3840);
    auto cylinder=plane;cylinder.mode=WarpMode::Cylinder;cylinder.curvature=0;WarpMapping zero(cylinder,2160,3840);
    for(int i=0;i<=20;++i){Point uv{i/20.,.2};auto a=reference.analytic(uv),b=zero.analytic(uv);require(std::hypot(a.x-b.x,a.y-b.y)<1e-9,"Zero curvature does not reduce to plane");}
    cylinder.curvature=100;cylinder.horizontalAxis=false;cylinder.yaw=0;WarpMapping aroundVertical(cylinder,2160,3840);
    auto tl=aroundVertical.analytic({0,0}),tr=aroundVertical.analytic({1,0}),tm=aroundVertical.analytic({.5,0});
    require(std::abs(tm.y-(tl.y+tr.y)/2)>1e-4,"Cylinder does not bend horizontal lines");
    cylinder.horizontalAxis=true;WarpMapping aroundHorizontal(cylinder,2160,3840);
    auto top=aroundHorizontal.analytic({0,0}),bottom=aroundHorizontal.analytic({0,1}),middle=aroundHorizontal.analytic({0,.5});
    require(std::abs(middle.x-(top.x+bottom.x)/2)>1e-4,"Horizontal cylinder does not bend vertical lines");
    cylinder.curvature=-100;WarpMapping convex(cylinder,2160,3840);require(convex.fit>0,"Reverse curvature not supported");
    auto mesh=legacy;mesh.offsets[5]={.025,-.025};mesh.offsets[10]={-.012,.018};WarpMapping m(mesh,2160,3840);
    for(int j=0;j<=7;++j)for(int i=0;i<=7;++i){Point uv{(i+.31)/8,(j+.23)/8};auto p=m.map(uv);
        // Recover source UV using the exact GPU triangle's barycentric map.
        int x=std::min(int(uv.x*m.divisions),m.divisions-1),y=std::min(int(uv.y*m.divisions),m.divisions-1);
        double fx=uv.x*m.divisions-x,fy=uv.y*m.divisions-y;
        auto a=std::uint32_t(y*(m.divisions+1)+x),b=a+1,c=a+m.divisions+1,d=c+1;
        auto va=m.vertices[fx+fy<=1?a:b],vb=m.vertices[fx+fy<=1?b:d],vc=m.vertices[c];
        double det=(vb.x-va.x)*(vc.y-va.y)-(vb.y-va.y)*(vc.x-va.x);
        double u=((p.x-va.x)*(vc.y-va.y)-(p.y-va.y)*(vc.x-va.x))/det;
        double v=((vb.x-va.x)*(p.y-va.y)-(vb.y-va.y)*(p.x-va.x))/det;
        Point recovered{va.u+u*(vb.u-va.u)+v*(vc.u-va.u),va.v+u*(vb.v-va.v)+v*(vc.v-va.v)};
        require(std::hypot((recovered.x-uv.x)*2160,(recovered.y-uv.y)*3840)<.003,"Cursor and GPU source sampling disagree");
    }
    auto bad=legacy;bad.offsets[0]={-.5,0};bool rejected=false;try{WarpMapping invalid(bad,2160,3840);}catch(...){rejected=true;}require(rejected,"Offscreen mesh accepted");
    bad=legacy;bad.offsets[5]={1,-1};rejected=false;try{WarpMapping invalid(bad,2160,3840);}catch(...){rejected=true;}require(rejected,"Folded mesh accepted");
    bad=plane;bad.distanceCm=0;rejected=false;try{WarpMapping invalid(bad,2160,3840);}catch(...){rejected=true;}require(rejected,"Invalid viewing distance accepted");
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
    log<<"PASS: "<<assertions<<" new warp assertions; plane/cylinder, both axes, reverse/zero curvature, portrait/landscape, offsets, mesh validity, GPU-barycentric cursor alignment. Runtime "<<elapsed<<" ms.\n";
}
