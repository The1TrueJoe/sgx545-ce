@frag loop_uf_m
varying mediump vec4 v_coords;
uniform highp float uf_third;
uniform highp float uf_one;
void main() { mediump vec4 res = v_coords; for (highp float ndx = 0.05; ndx < uf_one; ndx += uf_third) res = res.yzwx; gl_FragColor = vec4(res.rgb, 1.0); }
@frag loop_ui_m
varying mediump vec4 v_coords;
uniform mediump int ui_three;
void main() { mediump vec4 res = v_coords; for (mediump int ndx = 0; ndx < ui_three; ndx++) res = res.yzwx; gl_FragColor = vec4(res.rgb, 1.0); }
@frag loop_sel_count_m
varying mediump vec4 v_coords;
uniform bool ub_true;
void main() { mediump vec4 res = v_coords; for (int i = 0; i < (ub_true ? 3 : 0); i++) res = res.yzwx; gl_FragColor = vec4(res.rgb, 1.0); }
@frag loop_mixed_m
varying mediump vec4 v_coords;
void main() { mediump vec4 res = v_coords; for (mediump int i = 0; i < 5; i++) { if (i == 0) continue; else if (i == 3) break; res = res.yzwx; } gl_FragColor = vec4(res.rgb, 1.0); }
@frag loop_tricky2_m
varying mediump vec4 v_coords;
uniform mediump int ui_two; uniform mediump int ui_three;
void main() { mediump vec4 coords = v_coords; mediump vec4 res = coords; mediump int i = 0;
  while (i++ < ui_three) { res = coords.wxyz; mediump int j = 0; while (j++ < ui_two) res = res.yzwx; coords = res; }
  gl_FragColor = vec4(res.rgb, 1.0); }
@frag discard_dyn_m
varying mediump vec4 v_color; varying mediump vec4 v_coords;
void main() { gl_FragColor = v_color; if (v_coords.x + v_coords.y > 0.0) discard; }
@frag ret_cond_m
varying mediump vec4 v_coords;
mediump vec4 getColor() { if (v_coords.x + v_coords.y >= 0.0) return vec4(v_coords.xyz, 1.0); return vec4(v_coords.wzy, 1.0); }
void main() { gl_FragColor = getColor(); }
@frag ret_write_m
varying mediump vec4 v_coords;
void main() { gl_FragColor = vec4(v_coords.xyz, 1.0); if (v_coords.x + v_coords.y >= 0.0) return; gl_FragColor = vec4(v_coords.wzy, 1.0); }
@frag sel_iter_m
precision mediump float;
varying mediump vec4 v_coords;
uniform bool ub_true;
void main() { mediump vec4 res = v_coords; for (int i = 0; i < (ub_true ? 3 : 0); i++) res = res.yzwx; gl_FragColor = vec4(res.rgb, 1.0); }
@frag loop_sel_count_hi_m
varying mediump vec4 v_coords;
uniform bool ub_true;
void main() { mediump vec4 res = v_coords; for (highp int i = 0; i < (ub_true ? 3 : 0); i++) res = res.yzwx; gl_FragColor = vec4(res.rgb, 1.0); }
