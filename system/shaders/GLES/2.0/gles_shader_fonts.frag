/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#version 100

precision mediump float;
uniform sampler2D m_samp0;
varying vec4 m_cord0;
varying lowp vec4 m_colour;
uniform float m_sdrPeak;

#if defined(KODI_HDR_GUI_FULL)
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define PQ_PREC highp
#else
#define PQ_PREC mediump
#endif
// SDR (BT.709 sRGB) GUI -> BT.2020 PQ, mirroring gles_gui_composite.frag so the
// Amlogic direct-to-OSD-plane path matches the FBO composite path used
// elsewhere. peak = GUI white luminance, PQ-normalised (nits / 10000).
PQ_PREC vec3 pq_encode_gui(PQ_PREC vec3 c, PQ_PREC float peak)
{
  PQ_PREC vec3 l = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
  l = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * l;
  l = clamp(l * peak, 0.0, 1.0);
  PQ_PREC vec3 p = pow(l, vec3(0.1593017578125));
  return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));
}
#endif

void main ()
{
  vec4 rgb;

  rgb.rgb = m_colour.rgb;
  rgb.a = m_colour.a * texture2D(m_samp0, m_cord0.xy).a;

#if defined(KODI_HDR_GUI_FULL)
  rgb.rgb = pq_encode_gui(rgb.rgb, m_sdrPeak);
#endif

#if defined(KODI_LIMITED_RANGE)
  rgb.rgb *= (235.0 - 16.0) / 255.0;
  rgb.rgb += 16.0 / 255.0;
#endif

#if defined(KODI_TRANSFER_PQ) && !defined(KODI_HDR_GUI_FULL)
  rgb.rgb *= m_sdrPeak;
#endif

  gl_FragColor = rgb;
}
