#ifndef PT_BSDF_GLSL
#define PT_BSDF_GLSL

// A Principled-style layered BSDF: diffuse base, dielectric specular coat,
// metal, and microfacet transmission, all GGX / Smith with VNDF sampling.
// Radiance transport: refraction divides by eta^2, so a lossless dielectric
// conserves energy over an enter / exit pair (the glass furnace relies on it).
//
// Conventions: local frame with the geometric normal on +z. wo points toward
// the previous vertex (camera side), wi toward the next one. "entering" means
// wo is on the +z side, i.e. the ray arrives from outside the surface.

const float PT_PI = 3.14159265358979323846;
const float PT_INV_PI = 0.31830988618379067154;
const float PT_DELTA_ROUGHNESS = 1.0e-3;

struct PtMaterial {
    vec3 base_color;
    float metallic;
    float roughness;
    float ior;
    float transmission;
};

struct PtFrame {
    vec3 t;
    vec3 b;
    vec3 n;
};

PtFrame ptMakeFrame(vec3 n) {
    vec3 helper = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    PtFrame f;
    f.n = n;
    f.t = normalize(cross(helper, n));
    f.b = cross(n, f.t);
    return f;
}

vec3 ptToLocal(PtFrame f, vec3 v) { return vec3(dot(v, f.t), dot(v, f.b), dot(v, f.n)); }
vec3 ptToWorld(PtFrame f, vec3 v) { return f.t * v.x + f.b * v.y + f.n * v.z; }

// Exact unpolarized dielectric Fresnel. eta = n_transmitted / n_incident.
float ptFresnelDielectric(float cosThetaI, float eta) {
    cosThetaI = clamp(cosThetaI, 0.0, 1.0);
    float sin2ThetaT = (1.0 - cosThetaI * cosThetaI) / (eta * eta);
    if (sin2ThetaT >= 1.0) {
        return 1.0; // total internal reflection
    }
    float cosThetaT = sqrt(1.0 - sin2ThetaT);
    float rs = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
    float rp = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
    return 0.5 * (rs * rs + rp * rp);
}

vec3 ptFresnelSchlick(vec3 f0, float cosTheta) {
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

float ptGgxAlpha(float roughness) {
    return max(roughness * roughness, 1.0e-4);
}

float ptGgxD(float cosThetaH, float alpha) {
    if (cosThetaH <= 0.0) return 0.0;
    float a2 = alpha * alpha;
    float d = cosThetaH * cosThetaH * (a2 - 1.0) + 1.0;
    return a2 / (PT_PI * d * d);
}

float ptSmithLambda(float cosTheta, float alpha) {
    float c2 = cosTheta * cosTheta;
    float tan2 = (1.0 - c2) / max(c2, 1.0e-8);
    return 0.5 * (sqrt(1.0 + alpha * alpha * tan2) - 1.0);
}

float ptSmithG1(float cosTheta, float alpha) {
    return 1.0 / (1.0 + ptSmithLambda(abs(cosTheta), alpha));
}

float ptSmithG2(float cosThetaI, float cosThetaO, float alpha) {
    return 1.0 / (1.0 + ptSmithLambda(abs(cosThetaI), alpha) +
                  ptSmithLambda(abs(cosThetaO), alpha));
}

// Sample the GGX distribution of visible normals (isotropic alpha): E. Heitz,
// "Sampling the GGX Distribution of Visible Normals", JCGT 7(4), 2018. v is in
// the local frame with z up and must satisfy v.z > 0.
vec3 ptSampleGgxVndf(vec3 v, float alpha, float u1, float u2) {
    vec3 vh = normalize(vec3(alpha * v.x, alpha * v.y, v.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq)
                          : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u1);
    float phi = 2.0 * PT_PI * u2;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

vec3 ptCosineHemisphere(float u1, float u2) {
    float r = sqrt(u1);
    float phi = 2.0 * PT_PI * u2;
    return vec3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u1)));
}

// Lobe weights as in the Principled BSDF: metal replaces everything below
// it, transmission replaces the diffuse base under the dielectric coat.
struct PtLobes {
    float diffuse;
    float specular;
    float metal;
    float transmission;
};

PtLobes ptLobes(PtMaterial m) {
    float dielectric = 1.0 - m.metallic;
    PtLobes l;
    l.metal = m.metallic;
    l.transmission = dielectric * m.transmission;
    l.specular = dielectric;
    l.diffuse = dielectric * (1.0 - m.transmission);
    return l;
}

// Lobe selection probabilities. They only need to be a reasonable guess; the
// estimator stays unbiased as long as ptEval reports the same mixture pdf.
struct PtLobeProbabilities {
    float diffuse;
    float reflect;   // dielectric specular + metal + transmission's reflection
    float transmit;  // transmission's refraction
};

PtLobeProbabilities ptLobeProbabilities(PtMaterial m, PtLobes l, bool entering, float fresnelO) {
    PtLobeProbabilities p;
    if (entering) {
        p.diffuse = l.diffuse * (1.0 - fresnelO);
        p.reflect = l.specular * fresnelO + l.metal;
        p.transmit = l.transmission * (1.0 - fresnelO);
    } else {
        p.diffuse = 0.0;
        p.reflect = l.transmission * fresnelO;
        p.transmit = l.transmission * (1.0 - fresnelO);
    }
    float sum = p.diffuse + p.reflect + p.transmit;
    if (sum <= 0.0) return p;
    p.diffuse /= sum;
    p.reflect /= sum;
    p.transmit /= sum;
    return p;
}

// Evaluate the non-delta BSDF and the pdf of ptSample generating wi.
vec3 ptEval(PtMaterial m, vec3 wo, vec3 wi, out float pdf) {
    pdf = 0.0;
    PtLobes l = ptLobes(m);
    float alpha = ptGgxAlpha(m.roughness);
    float cosO = wo.z;
    float cosI = wi.z;
    bool entering = cosO > 0.0;
    float eta = entering ? m.ior : 1.0 / m.ior;
    float fresnelO = ptFresnelDielectric(abs(cosO), eta);
    PtLobeProbabilities p = ptLobeProbabilities(m, l, entering, fresnelO);
    if (abs(cosO) < 1.0e-6 || abs(cosI) < 1.0e-6) return vec3(0.0);
    if (m.roughness < PT_DELTA_ROUGHNESS) {
        // Delta coat / mirror / glass: only the diffuse base can be evaluated.
        if (!entering || cosI <= 0.0 || l.diffuse <= 0.0) return vec3(0.0);
        pdf = p.diffuse * cosI * PT_INV_PI;
        return l.diffuse * m.base_color * PT_INV_PI * (1.0 - fresnelO);
    }

    if (cosI * cosO > 0.0) {
        // Reflection on wo's side of the surface.
        vec3 h = normalize(wo + wi);
        if (!entering) h = -h; // keep h on the +z side for D
        float woH = abs(dot(wo, h));
        float d = ptGgxD(h.z, alpha);
        float g2 = ptSmithG2(cosI, cosO, alpha);
        float g1 = ptSmithG1(cosO, alpha);
        float specCommon = d * g2 / (4.0 * abs(cosI) * abs(cosO));
        float pdfSpec = d * g1 / (4.0 * abs(cosO));
        float fh = ptFresnelDielectric(woH, eta);
        vec3 f = vec3(0.0);
        if (entering) {
            f += l.diffuse * m.base_color * PT_INV_PI * (1.0 - fh);
            f += l.specular * fh * specCommon;
            f += l.metal * ptFresnelSchlick(m.base_color, woH) * specCommon;
            pdf = p.diffuse * abs(cosI) * PT_INV_PI + p.reflect * pdfSpec;
        } else {
            f += l.transmission * fh * specCommon;
            pdf = p.reflect * pdfSpec;
        }
        return f;
    }

    // Refraction through the surface (Walter et al. 2007), radiance transport.
    if (l.transmission <= 0.0) return vec3(0.0);
    float etaI = entering ? 1.0 : m.ior;
    float etaT = entering ? m.ior : 1.0;
    vec3 h = normalize(wo * etaI + wi * etaT);
    if (h.z < 0.0) h = -h;
    float woH = dot(wo, h);
    float wiH = dot(wi, h);
    if (woH * wiH >= 0.0) return vec3(0.0);
    float d = ptGgxD(h.z, alpha);
    float g2 = ptSmithG2(cosI, cosO, alpha);
    float g1 = ptSmithG1(cosO, alpha);
    float fh = ptFresnelDielectric(abs(woH), eta);
    float denom = etaI * woH + etaT * wiH;
    denom = denom * denom;
    float jac = etaT * etaT * abs(wiH) / denom;
    // Walter's BTDF carries etaT^2; dividing by (etaT/etaI)^2 turns it into
    // the radiance-transport form used by unidirectional path tracing.
    vec3 f = l.transmission * (1.0 - fh) * m.base_color *
             (abs(woH) * abs(wiH) * d * g2 / (abs(cosI) * abs(cosO) * denom)) *
             (etaI * etaI);
    pdf = p.transmit * d * g1 * abs(woH) / abs(cosO) * jac;
    return f;
}

// Sample wi from wo. Returns the path weight f * |cos| / pdf. For delta
// lobes pdf is reported as 0 and the weight is the exact lobe value.
vec3 ptSample(PtMaterial m, vec3 wo, float u0, float u1, float u2,
              out vec3 wi, out float pdf, out bool isDelta) {
    PtLobes l = ptLobes(m);
    float alpha = ptGgxAlpha(m.roughness);
    bool delta = m.roughness < PT_DELTA_ROUGHNESS;
    float cosO = wo.z;
    bool entering = cosO > 0.0;
    float eta = entering ? m.ior : 1.0 / m.ior;
    float fresnelO = ptFresnelDielectric(abs(cosO), eta);
    PtLobeProbabilities p = ptLobeProbabilities(m, l, entering, fresnelO);
    isDelta = false;
    pdf = 0.0;
    wi = vec3(0.0, 0.0, 1.0);
    if (p.diffuse + p.reflect + p.transmit <= 0.0) return vec3(0.0);

    vec3 n = vec3(0.0, 0.0, entering ? 1.0 : -1.0);
    vec3 woSide = entering ? wo : -wo;

    if (delta) {
        // Perfect specular coat / mirror / glass. The diffuse base is still a
        // regular lobe; everything else is a delta lobe chosen by Fresnel.
        if (u0 < p.diffuse) {
            vec3 local = ptCosineHemisphere(u1, u2);
            wi = local;
            pdf = p.diffuse * local.z * PT_INV_PI;
            vec3 f = l.diffuse * m.base_color * PT_INV_PI * (1.0 - fresnelO);
            return f * abs(wi.z) / max(pdf, 1.0e-8);
        }
        isDelta = true;
        float etaI = entering ? 1.0 : m.ior;
        float etaT = entering ? m.ior : 1.0;
        if (u0 < p.diffuse + p.reflect) {
            wi = vec3(-wo.x, -wo.y, wo.z);
            vec3 value = vec3(0.0);
            if (entering) {
                value += vec3(l.specular * fresnelO);
                value += l.metal * ptFresnelSchlick(m.base_color, abs(cosO));
            } else {
                value += vec3(l.transmission * fresnelO);
            }
            return value / max(p.reflect, 1.0e-8);
        }
        vec3 refracted = refract(-woSide, n, 1.0 / eta);
        if (dot(refracted, refracted) == 0.0) {
            // Total internal reflection: the Fresnel term already said 1.
            wi = vec3(-wo.x, -wo.y, wo.z);
            return vec3(l.transmission) / max(p.transmit, 1.0e-8);
        }
        wi = refracted;
        vec3 value = l.transmission * (1.0 - fresnelO) * m.base_color *
                     ((etaI * etaI) / (etaT * etaT));
        return value / max(p.transmit, 1.0e-8);
    }

    if (u0 < p.diffuse) {
        wi = ptCosineHemisphere(u1, u2);
    } else if (u0 < p.diffuse + p.reflect) {
        vec3 h = ptSampleGgxVndf(woSide, alpha, u1, u2);
        vec3 r = reflect(-woSide, h);
        if (r.z <= 0.0) return vec3(0.0);
        wi = entering ? r : -r;
    } else {
        vec3 h = ptSampleGgxVndf(woSide, alpha, u1, u2);
        vec3 refracted = refract(-woSide, h, 1.0 / eta);
        if (dot(refracted, refracted) == 0.0) return vec3(0.0);
        wi = entering ? refracted : -refracted;
        if (wi.z * wo.z >= 0.0) return vec3(0.0);
    }
    vec3 f = ptEval(m, wo, wi, pdf);
    if (pdf <= 0.0) return vec3(0.0);
    return f * abs(wi.z) / pdf;
}

#endif
