/*
   Using deterministic logic (Bayer matrices, arithmetic hashing) 
   to simulate physical chaos (paper grain, ink bleed).
   Adapted for Umbriel Wayland Compositor.
   Originally by @snes19xx, https://github.com/snes19xx
   All credits go to them.
*/

// 4x4 Bayer Matrix, dithering
float getBayer(vec2 pos) {
    int x = int(clamp(mod(pos.x, 4.0), 0.0, 3.0));
    int y = int(clamp(mod(pos.y, 4.0), 0.0, 3.0));
    const mat4 bayer = mat4(
        0.0, 12.0,  3.0, 15.0,
        8.0,  4.0, 11.0,  7.0,
        2.0, 14.0,  1.0, 13.0,
        10.0,  6.0,  9.0,  5.0
    );
    return bayer[x][y] / 16.0;
}

// "Hash12"
// (https://www.shadertoy.com/view/4djSRW)
float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Multi-octave noise for realistic paper fiber texture
float paperTexture(vec2 uv) {
    float n = 0.0;
    n += hash(uv * 0.3) * 0.6;       // Very large fibers
    n += hash(uv * 0.8) * 0.4;       // Large fibers
    n += hash(uv * 2.5) * 0.3;       // Medium detail
    n += hash(uv * 6.0) * 0.2;       // Fine grain
    n += hash(uv * 15.0) * 0.1;      // Very fine grain
    return n / 1.6;
}

// Directional paper grain (simulates paper fibers running in one direction)
float directionalGrain(vec2 uv) {
    vec2 direction = vec2(0.7, 0.3); // Fiber direction
    float grain = 0.0;
    grain += hash(uv * 3.0 + direction * 2.0) * 0.5;
    grain += hash(uv * 8.0 + direction * 5.0) * 0.3;
    return grain / 0.8;
}

// Subtle vignette for paper edge darkening
float vignette(vec2 uv) {
    vec2 center = uv - 0.5;
    float dist = length(center);
    return 1.0 - smoothstep(0.4, 1.2, dist) * 0.15;
}

// Umbriel screen effect entrypoint
vec4 screen(vec2 uv) {
    vec4 pixColor = umbriel_sample(uv);
    
    // Luma Conversion
    float gray = dot(pixColor.rgb, vec3(0.299, 0.587, 0.114));
    
    // E-ink characteristic response curve
    gray = pow(gray, 1.2);
    
    // Better contrast with slight S-curve
    gray = smoothstep(0.08, 0.92, gray);
    
    // Mid-tone boost
    float midBoost = smoothstep(0.3, 0.5, gray) * (1.0 - smoothstep(0.5, 0.7, gray));
    gray += midBoost * 0.1;
    
    // Physical pixel position aligned with output scale
    vec2 screenPos = uv * umbriel_size * (umbriel_scale > 0.0 ? umbriel_scale : 1.0);
    
    // Paper grain
    float paperGrain = (paperTexture(screenPos * 0.3) - 0.5) * 0.035; 
    float dirGrain = (directionalGrain(screenPos * 0.4) - 0.5) * 0.025;
    
    float bayerValue = getBayer(screenPos);
    
    // Apply to bright areas (paper), and slightly to mid-tones
    float textureMask = smoothstep(0.5, 0.95, gray);
    
    // Apply both grain types
    gray += paperGrain * textureMask;
    gray += dirGrain * textureMask * 0.7;
    
    // Dithering for paper texture
    float ditherStrength = 0.025;
    gray += (bayerValue - 0.5) * ditherStrength * textureMask;
    
    // Vignette for paper edges
    float vig = vignette(uv);
    gray *= vig;
    
    gray = clamp(gray, 0.0, 1.0);
    
    // E-ink colors with slight warmth variation
    vec3 paperColor = vec3(0.94, 0.92, 0.86);
    vec3 inkColor   = vec3(0.10, 0.10, 0.12);
    
    // Texture color variation
    float colorVariation = hash(screenPos * 0.08) * 0.02;
    paperColor += vec3(colorVariation, colorVariation * 0.5, -colorVariation * 0.2);
    
    // Map gray value to paper & ink palette
    vec3 finalColor = mix(inkColor, paperColor, gray);
    
    return vec4(finalColor, pixColor.a);
}

// Window effect entrypoint (allows applying per-window if desired)
vec4 window(vec2 uv) {
    return screen(uv);
}
