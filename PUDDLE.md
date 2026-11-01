# Road puddle feature

The puddle renderer generates its own 512×512 irregular, multi-lobed dark mask;
no copied oval texture or shader asset folder is needed. GTA's RenderWare shadow
queue draws the puddle body. The separate additive ring texture is disabled
because SA Android rendered it as bright outline markings. Per-puddle footprint
and rotation vary, and spawns prefer visible nearby locations.

Optional AML configuration:

```ini
[Puddle]
Enabled=1
MaxPuddles=2
SpawnIntervalMs=12000
SpawnMinDistance=10.0
SpawnRadius=55.0
CleanupRadius=85.0
LifeMinMs=35000
LifeMaxMs=70000
ReflectionEnabled=1
ReflectionDistance=14.0
```

Each puddle has an independent lifetime and fades out before removal. When
`ReflectionEnabled=1`, the mod hooks the GLES draw calls and runs the fake-SSR
pass on the render thread when the puddle texture is drawn. It requires a GLES 3
context; on GLES 2 or if the hooks/shader cannot initialize, the puddle keeps
using the native RenderWare shadow instead. `ReflectionDistance` controls the
approximate virtual reflection distance in world units; this is screen-space
fake-SSR, not depth-buffer SSR.

References checked:

- WaterRipple muddy-road effect: https://www.gtainside.com/sanandreas/mods/122060-waterripple/
- Small puddles rain reference: https://www.gtainside.com/sanandreas/mods/198267-rain-mods-by-trushinvlad-updated-by-artemqa146
- Built-in water object catalog: https://citwiki.com/CITtools/Objects/category/water/
