#pragma once
#include "geometry.h"
#include <vector>
#include <cstdint>
#include <limits>

namespace sw {
enum class WarpMode { Corners=0, ViewPlane=1, Cylinder=2 };
struct Vec3 {
    double x{},y{},z{};
    Vec3 operator+(Vec3 b)const{return{x+b.x,y+b.y,z+b.z};}
    Vec3 operator-(Vec3 b)const{return{x-b.x,y-b.y,z-b.z};}
    Vec3 operator*(double s)const{return{x*s,y*s,z*s};}
};
inline double Dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 Cross(Vec3 a,Vec3 b){return{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline Vec3 Unit(Vec3 a){double n=std::sqrt(Dot(a,a));if(n<1e-12)throw std::runtime_error("Invalid view direction");return a*(1/n);}
struct WarpProfile {
    Quad corners=IdentityQuad();
    WarpMode mode=WarpMode::Corners;
    double widthCm=30,heightCm=53.333333,distanceCm=60;
    double yaw=30,pitch=0,eyeX=0,eyeY=0,curvature=100;
    bool horizontalAxis=false;
    std::array<Point,16> offsets{}; // cubic Bezier displacement control lattice
};
inline std::array<double,4> Bernstein(double t){double s=1-t;return{s*s*s,3*t*s*s,3*t*t*s,t*t*t};}
inline bool HasResidual(const WarpProfile& p){for(auto d:p.offsets)if(std::abs(d.x)>1e-12||std::abs(d.y)>1e-12)return true;return false;}
inline void ValidateProfile(const WarpProfile& p){
    auto in=[](double v,double a,double b){return std::isfinite(v)&&v>=a&&v<=b;};
    if(!ValidQuad(p.corners)||int(p.mode)<0||int(p.mode)>2||!in(p.widthCm,5,200)||!in(p.heightCm,5,200)||!in(p.distanceCm,15,300)||
       !in(p.yaw,-65,65)||!in(p.pitch,-45,45)||!in(p.eyeX,-150,150)||!in(p.eyeY,-150,150)||!in(p.curvature,-100,100))
        throw std::runtime_error("Invalid calibration parameters");
    for(auto d:p.offsets)if(!in(d.x,-1,1)||!in(d.y,-1,1))throw std::runtime_error("Invalid mesh displacement");
}
struct WarpVertex {float x,y,u,v;};
class WarpMapping {
public:
    WarpProfile profile;
    Homography homography;
    bool projective{};
    bool residual{};
    double fit=1;
    int divisions=0;
    std::uint64_t revision{};
    std::vector<WarpVertex> vertices;
    std::vector<std::uint32_t> indices;
    WarpMapping(const WarpProfile& p,int width,int height):profile(p),homography(FromQuad(p.corners)),pixelWidth(width),pixelHeight(height){
        ValidateProfile(p);if(width<1||height<1)throw std::runtime_error("Invalid display size");
        static std::uint64_t nextRevision=0;revision=++nextRevision;
        residual=HasResidual(p);projective=p.mode==WarpMode::Corners&&!residual;
        const double rad=3.14159265358979323846/180;
        double cy=std::cos(p.yaw*rad),sy=std::sin(p.yaw*rad),cp=std::cos(p.pitch*rad),sp=std::sin(p.pitch*rad);
        axisX={cy,0,sy};axisY={-sy*sp,cp,cy*sp};normal=Cross(axisX,axisY);
        center={0,0,p.distanceCm};eye={p.eyeX,p.eyeY,0};forward=Unit(center-eye);
        right=Unit(Cross({0,1,0},forward));down=Cross(forward,right);distance=std::sqrt(Dot(center-eye,center-eye));
        if(p.mode!=WarpMode::Corners){
            if(Dot(normal,center-eye)<=distance*.08)throw std::runtime_error("The screen faces away from the eye; reduce angle or eye offset");
            if(!Fits(1)){double lo=0,hi=1;for(int i=0;i<32;++i){double mid=(lo+hi)/2;if(Fits(mid))lo=mid;else hi=mid;}fit=lo;}
            fit*=.995;
            if(fit<.03)throw std::runtime_error("The selected view leaves too little usable screen area");
        }
        if(!projective){
            for(int n:{16,32,64,128,256}){
                Build(n);if(ApproximationError()<=.20)break;
                if(n==256)throw std::runtime_error("Warp is too extreme for accurate cursor alignment");
            }
            ValidateMesh();
        }
    }
    // Exact projective path for existing profiles; the mesh path uses the SAME
    // piecewise affine map as GPU texture interpolation, including the diagonal.
    Point map(Point p) const {
        if(projective)return homography.map(p);
        double fx=std::clamp(p.x,0.,1.)*divisions,fy=std::clamp(p.y,0.,1.)*divisions;
        int x=std::min(int(fx),divisions-1),y=std::min(int(fy),divisions-1);fx-=x;fy-=y;
        auto a=Vertex(x,y),b=Vertex(x+1,y),c=Vertex(x,y+1),d=Vertex(x+1,y+1);
        if(fx+fy<=1)return{a.x+(b.x-a.x)*fx+(c.x-a.x)*fy,a.y+(b.y-a.y)*fx+(c.y-a.y)*fy};
        return{d.x+(c.x-d.x)*(1-fx)+(b.x-d.x)*(1-fy),d.y+(c.y-d.y)*(1-fx)+(b.y-d.y)*(1-fy)};
    }
    Point analytic(Point p) const {
        Point out=profile.mode==WarpMode::Corners?homography.map(p):Physical(p,fit);
        if(!residual)return out;
        auto bx=Bernstein(p.x),by=Bernstein(p.y);
        for(int j=0;j<4;++j)for(int i=0;i<4;++i){auto d=profile.offsets[j*4+i];double w=bx[i]*by[j];out.x+=d.x*w;out.y+=d.y*w;}
        return out;
    }
private:
    int pixelWidth,pixelHeight;
    Vec3 axisX,axisY,normal,center,eye,forward,right,down;
    double distance{};
    Point Physical(Point p,double scale) const {
        double x=(p.x-.5)*profile.widthCm*scale,y=(p.y-.5)*profile.heightCm*scale,z=0;
        if(profile.mode==WarpMode::Cylinder&&std::abs(profile.curvature)>1e-8){
            double k=profile.curvature*.01/distance;
            double& coordinate=profile.horizontalAxis?y:x;double angle=coordinate*k;
            if(std::abs(angle)>1.35)throw std::runtime_error("Cylinder arc exceeds the visible range");
            z=-2*std::sin(angle/2)*std::sin(angle/2)/k;coordinate=std::sin(angle)/k;
        }
        Vec3 virtualPoint=center+right*x+down*y+forward*z,ray=virtualPoint-eye;
        double denominator=Dot(normal,ray);
        if(denominator<=1e-8)throw std::runtime_error("A view ray does not intersect the front of the screen");
        double lambda=Dot(normal,center-eye)/denominator;
        Vec3 p3=eye+ray*lambda-center;
        return{.5+Dot(p3,axisX)/profile.widthCm,.5+Dot(p3,axisY)/profile.heightCm};
    }
    bool Fits(double scale) const {
        try{for(int j=0;j<=8;++j)for(int i=0;i<=32;++i){auto p=Physical({i/32.,j/8.},scale);if(p.x<0||p.x>1||p.y<0||p.y>1)return false;}}catch(...){return false;}return true;
    }
    Point Vertex(int x,int y)const{const auto& v=vertices[size_t(y)*(divisions+1)+x];return{v.x,v.y};}
    void Build(int n){
        divisions=n;vertices.clear();indices.clear();vertices.reserve(size_t(n+1)*(n+1));indices.reserve(size_t(n)*n*6);
        for(int y=0;y<=n;++y)for(int x=0;x<=n;++x){Point uv{x/double(n),y/double(n)},p=analytic(uv);vertices.push_back({float(p.x),float(p.y),float(uv.x),float(uv.y)});}
        for(int y=0;y<n;++y)for(int x=0;x<n;++x){auto a=std::uint32_t(y*(n+1)+x),b=a+1,c=a+n+1,d=c+1;indices.insert(indices.end(),{a,b,c,b,d,c});}
    }
    double ApproximationError()const{
        double worst=0;
        for(int y=0;y<divisions;++y)for(int x=0;x<divisions;++x)for(Point sample:std::array<Point,3>{{{.5,.5},{1./3,1./3},{2./3,2./3}}}){
            Point uv{(x+sample.x)/divisions,(y+sample.y)/divisions};auto exact=analytic(uv),raster=map(uv);
            worst=std::max(worst,std::hypot((exact.x-raster.x)*pixelWidth,(exact.y-raster.y)*pixelHeight));
        }return worst;
    }
    static double Area(Point a,Point b,Point c){return(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
    static bool OnSegment(Point a,Point b,Point p){return std::abs(Area(a,b,p))<1e-12&&p.x>=std::min(a.x,b.x)-1e-12&&p.x<=std::max(a.x,b.x)+1e-12&&p.y>=std::min(a.y,b.y)-1e-12&&p.y<=std::max(a.y,b.y)+1e-12;}
    void ValidateMesh()const{
        for(auto v:vertices)if(!std::isfinite(v.x)||!std::isfinite(v.y)||v.x<-.0000001||v.x>1.0000001||v.y<-.0000001||v.y>1.0000001)
            throw std::runtime_error("A mesh handle would move content outside the screen");
        for(size_t i=0;i<indices.size();i+=3){auto a=vertices[indices[i]],b=vertices[indices[i+1]],c=vertices[indices[i+2]];
            if(Area({a.x,a.y},{b.x,b.y},{c.x,c.y})<1e-10)throw std::runtime_error("A mesh handle would fold or collapse the image");}
        // Positive triangle areas plus a simple boundary give a non-overlapping
        // piecewise-linear disk; check the actual rendered boundary, not a few
        // samples of the continuous Bezier Jacobian.
        std::vector<Point> boundary;
        for(int x=0;x<divisions;++x)boundary.push_back(Vertex(x,0));
        for(int y=0;y<divisions;++y)boundary.push_back(Vertex(divisions,y));
        for(int x=divisions;x>0;--x)boundary.push_back(Vertex(x,divisions));
        for(int y=divisions;y>0;--y)boundary.push_back(Vertex(0,y));
        size_t count=boundary.size();
        for(size_t i=0;i<count;++i)for(size_t j=i+2;j<count;++j){
            if(i==0&&j==count-1)continue;
            auto a=boundary[i],b=boundary[(i+1)%count],c=boundary[j],d=boundary[(j+1)%count];
            if((Area(a,b,c)*Area(a,b,d)<0&&Area(c,d,a)*Area(c,d,b)<0)||OnSegment(a,b,c)||OnSegment(a,b,d)||OnSegment(c,d,a)||OnSegment(c,d,b))throw std::runtime_error("Warp boundary intersects itself");
        }
    }
};
}
