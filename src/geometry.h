#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace sw {
struct Point { double x = 0, y = 0; };
using Quad = std::array<Point, 4>; // top left, top right, bottom right, bottom left
inline Quad IdentityQuad() { return {{{0,0},{1,0},{1,1},{0,1}}}; }
struct Homography {
    std::array<double, 9> m{1,0,0,0,1,0,0,0,1};
    Point map(Point p) const {
        const double z = m[6]*p.x + m[7]*p.y + m[8];
        if (std::abs(z) < 1e-10) throw std::runtime_error("Singular perspective transform");
        return {(m[0]*p.x+m[1]*p.y+m[2])/z, (m[3]*p.x+m[4]*p.y+m[5])/z};
    }
    Homography inverse() const {
        const auto& a=m;
        Homography b;
        b.m={a[4]*a[8]-a[5]*a[7],a[2]*a[7]-a[1]*a[8],a[1]*a[5]-a[2]*a[4],
             a[5]*a[6]-a[3]*a[8],a[0]*a[8]-a[2]*a[6],a[2]*a[3]-a[0]*a[5],
             a[3]*a[7]-a[4]*a[6],a[1]*a[6]-a[0]*a[7],a[0]*a[4]-a[1]*a[3]};
        double det=a[0]*b.m[0]+a[1]*b.m[3]+a[2]*b.m[6];
        if (std::abs(det)<1e-10) throw std::runtime_error("Degenerate quadrilateral");
        for(auto& v:b.m) v/=det;
        return b;
    }
};
inline bool ValidQuad(const Quad& q) {
    for(const auto& p:q) if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.x>1||p.y<0||p.y>1) return false;
    double area=0;
    for(int i=0;i<4;++i) {
        auto a=q[i], b=q[(i+1)%4], c=q[(i+2)%4];
        if((b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x)<0.002) return false;
        area+=a.x*b.y-b.x*a.y;
    }
    return area>0.08;
}
inline Homography FromQuad(const Quad& q) {
    if(!ValidQuad(q)) throw std::runtime_error("Invalid calibration quadrilateral");
    const auto src=IdentityQuad();
    double a[8][9]{};
    for(int i=0;i<4;++i) {
        double x=src[i].x,y=src[i].y,u=q[i].x,v=q[i].y;
        double r1[9]={x,y,1,0,0,0,-u*x,-u*y,u};
        double r2[9]={0,0,0,x,y,1,-v*x,-v*y,v};
        std::copy(r1,r1+9,a[i*2]); std::copy(r2,r2+9,a[i*2+1]);
    }
    for(int col=0;col<8;++col) {
        int pivot=col;
        for(int r=col+1;r<8;++r) if(std::abs(a[r][col])>std::abs(a[pivot][col])) pivot=r;
        if(std::abs(a[pivot][col])<1e-10) throw std::runtime_error("Singular calibration");
        for(int j=0;j<9;++j) std::swap(a[pivot][j],a[col][j]);
        const double d=a[col][col]; for(int j=col;j<9;++j) a[col][j]/=d;
        for(int r=0;r<8;++r) if(r!=col) {
            const double f=a[r][col]; for(int j=col;j<9;++j) a[r][j]-=f*a[col][j];
        }
    }
    Homography h; for(int i=0;i<8;++i) h.m[i]=a[i][8];
    return h;
}
}
