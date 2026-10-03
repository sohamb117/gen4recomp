# nitromdl

glTF 2.0 to NSBMD. One static triangle mesh, no skinning.

```
nitromdl in.gltf out.nsbmd
nitromdl in.glb  out.nsbmd
```

POSITION is required. COLOR_0 is optional; without it the mesh is magenta, so
an untextured prop still shows. A texture in the glTF is refused, because this
encoder does not write TEX0 yet and extra map props do not bind one.
