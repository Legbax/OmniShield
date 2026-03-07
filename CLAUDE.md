# OmniShield — Notas para Claude

OmniShield es un módulo Zygisk para Android que intercepta la capa Binder IPC
para enmascarar las coordenadas GPS reales del dispositivo. Funciona hookeando
`BBinder::transact`, `IPCThreadState::transact`, `JavaBBinder::onTransact` y
`Parcel::readDouble` con DobbyHook, reemplazando las coordenadas en tiempo de
lectura sin escribir en la memoria mmap de solo lectura del kernel binder.

## Arquitectura de spoofing (resumen)

- **BBinder DATA path**: Shadow buffer swap — copia writable del Parcel, muta
  coordenadas, intercambia puntero mData durante `orig_bbinder_transact`.
- **IPC REPLY path**: Arma `Parcel::readDouble` con coordenadas reales como
  clave. Cuando `readDouble` retorna un valor que coincide exactamente con las
  coordenadas reales almacenadas, devuelve las coordenadas spoofed en su lugar.
- **Detección de coordenadas**: `probeProviderValidatedLocation()` busca strings
  de provider conocidos ("gps"/"fused"/"network"/"passive") en UTF-16LE, luego
  escanea doubles consecutivos en rango lat/lon válido.

## Gists de GitHub

No uses `WebFetch` para leer gists de GitHub (gist.githubusercontent.com / gist.github.com). La herramienta no puede acceder a esos dominios. En su lugar usa bash:

```bash
# Descargar gist por URL raw
curl -sL "https://gist.githubusercontent.com/USER/ID/raw/FILE"

# O usar gh CLI
gh gist view <ID>
```
