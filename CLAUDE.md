# OmniShield — Notas para Claude

## Gists de GitHub

No uses `WebFetch` para leer gists de GitHub (gist.githubusercontent.com / gist.github.com). La herramienta no puede acceder a esos dominios. En su lugar usa bash:

```bash
# Descargar gist por URL raw
curl -sL "https://gist.githubusercontent.com/USER/ID/raw/FILE"

# O usar gh CLI
gh gist view <ID>
```
