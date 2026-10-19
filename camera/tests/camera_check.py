#!/usr/bin/env python3
"""Analizador de ajedrez 2x2 para previews/capturas de la camara mocha.

El binning 2x2 del IMX179 suma los cuatro fotodiodos de cada celda
Bayer: la lectura no tiene mosaico que desmosaizar, y cualquier
demosaico software convierte la imagen en un patron de ajedrez de
bloques 2x2 alternos. Este script mide esa firma:

  score_2x2 : contrate entre pares adyacentes (fila/col) y diagonales
              del patron 2x2.  Ajedrez fuerte => cerca de +1,
              imagen natural => cerca de 0.
  lag2      : energia de la diferencia lag-2 normalizada por lag-1.
              El ajedrez anula la lag-2 (repite el bloque), la imagen
              natural la mantiene alta.

Uso: camera_check.py imagen.png [imagen2.png ...]
Salida: metricas por imagen y veredicto AJEDREZ/LIMPIO.
"""
import sys

import numpy as np
from PIL import Image

# Calibracion empirica (mocha, Oct 2026):
#   LIMPIAS: frontal 720p -0.146 / trasera crop 720p -0.146 /
#            JPEG nativo 3264 +0.04..+0.05 (lag 0.86-1.66)
#   AJEDREZ (sintetico patron luminosidad RGGB, contraste 5%):
#            +0.47 (lag 0.35); contraste 10%: +0.65 (lag 0.21)
# Margen amplio: umbral 0.18 queda entre +0.06 (peor limpia) y
# +0.26 (ajedrez tenue); la condicion lag<0.55 evita falsos positivos
# por textura natural periodica.
UMBRAL_2X2 = 0.18
UMBRAL_LAG2 = 0.55


def analisis(path):
    g = np.asarray(Image.open(path).convert("L"), dtype=np.float64)
    g = g[: g.shape[0] // 2 * 2, : g.shape[1] // 2 * 2]
    a = g[0::2, 0::2]  # fase (0,0)
    b = g[0::2, 1::2]  # fase (0,1)
    c = g[1::2, 0::2]  # fase (1,0)
    d = g[1::2, 1::2]  # fase (1,1)

    adyacentes = (np.abs(a - b).mean() + np.abs(a - c).mean()
                  + np.abs(b - d).mean() + np.abs(c - d).mean()) / 4.0
    diagonales = (np.abs(a - d).mean() + np.abs(b - c).mean()) / 2.0
    score_2x2 = (adyacentes - diagonales) / (adyacentes + diagonales + 1e-6)

    d1h = np.abs(g[:, :-1] - g[:, 1:]).mean()
    d1v = np.abs(g[:-1, :] - g[1:, :]).mean()
    d2h = np.abs(g[:, :-2] - g[:, 2:]).mean()
    d2v = np.abs(g[:-2, :] - g[2:, :]).mean()
    lag1 = (d1h + d1v) / 2.0
    lag2 = (d2h + d2v) / 2.0
    ratio = lag2 / (lag1 + 1e-6)

    ajedrez = score_2x2 > UMBRAL_2X2 and ratio < UMBRAL_LAG2
    return score_2x2, ratio, ajedrez


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    peor = False
    for path in sys.argv[1:]:
        score, ratio, ajedrez = analisis(path)
        peor = peor or ajedrez
        print(f"{path}: score_2x2={score:+.4f} lag2/lag1={ratio:.3f} "
              f"=> {'AJEDREZ' if ajedrez else 'LIMPIO'}")
    return 1 if peor else 0


if __name__ == "__main__":
    sys.exit(main())
