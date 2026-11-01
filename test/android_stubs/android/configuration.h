#pragma once
struct AConfiguration { int density=160; };
inline int AConfiguration_getDensity(const AConfiguration *c){return c->density;}
inline constexpr int ACONFIGURATION_DENSITY_ANY=0xfffe;
