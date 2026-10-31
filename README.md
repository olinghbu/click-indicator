# Click Indicator v0.1.0-alpha.1 (Geode 5.9.0 · GD 2.2081)

Mod de Geode que te indica cuándo dar click. Por ahora solo se activa en **Slaughterhouse** (ID `27690100`, o cualquier nivel llamado "Slaughterhouse" si "Detectar copias" está activado).

## Qué muestra

| Elemento | Qué significa |
|---|---|
| Columnas verdes en el nivel | Cada franja va desde donde presionas hasta donde sueltas, de arriba a abajo de la pantalla. Línea verde brillante = presionar, línea verde más oscura = soltar. |
| Cuadritos | Dónde estaba el jugador al presionar (relleno) y al soltar (hueco). |
| Línea blanca | Posición actual del jugador. |
| Barra de abajo *(opcional, apagada por defecto)* | Los próximos clicks se acercan a la línea blanca. Bloque verde = mantener, línea verde = presionar, línea roja = soltar. Si hay 2 jugadores, se divide en 2 carriles (arriba P1, abajo P2). |
| Círculo *(opcional)* | Se llena cuando se acerca un click y se pone blanco con "CLICK!" en el momento exacto. Muestra "MANTEN" mientras tienes que mantener presionado. El anillo de afuera es tu input real: verde si coincide, rojo si no. |
| Precisión *(opcional)* | Al dar click: `perfecto`, `temprano 8ms`, `tarde 12ms` o `click de mas`. |

## Cómo conseguir los clicks

El mod **no trae los clicks de Slaughterhouse ya incluidos**. Tiene que sacarlos de un run válido:

1. **Grabando tu run** (activado por defecto): entra al nivel, activa el **modo práctica**, pon checkpoints y llega al 100%. Cuando mueres en un checkpoint, los inputs que vienen después de ese checkpoint se borran, así que lo que se guarda es el run "limpio". Al completar el nivel aparece *"run guardado"* y desde ese intento ya te guía. Se guarda en la carpeta de guardado del mod como `clicks_27690100.txt`.
2. **Importando un macro**: en ajustes → "Importar macro", elige un `.gdr.json` (formato GDR en JSON, el que exportan xdBot y otros bots) o un `.txt` de este mod. Los macros solo traen tiempos: las columnas se ubican por adelantado con la velocidad del jugador y se corrigen a la posición exacta cuando pasas por ahí (los cuadritos aparecen después de pasar).

Los tiempos van por ticks de física (240 por segundo), así que la guía no depende de tus FPS.

## Compilar

Igual que tu repo `gd-optimizacion-v5`:

1. Crea un repo nuevo en GitHub y sube esta carpeta.
2. GitHub Actions compila solo (`.github/workflows/build.yml`). El `.geode` aparece en **Actions → artifacts** y, si la rama es `main`, en **Releases → latest**.
3. Copia el `.geode` a `%LOCALAPPDATA%\GeometryDash\geode\mods\` (o la carpeta `geode\mods` de tu instalación de Steam) y reinicia el juego.

Para compilar en tu PC: `geode build` con el Geode SDK y la CLI instalados.

## Limitaciones

- Con **start pos** solo se ven las columnas del nivel (la barra se basa en el tiempo desde el 0%).
- Con **TPS bypass / physics bypass** los tiempos no coinciden; usa los 240 TPS normales.
- Si mantienes presionado justo en un checkpoint, la grabación corta ese hold en el respawn.
- Se compila para Windows y Android (32 y 64 bits) en un solo `.geode`. iOS/Mac todavía no.

Es una herramienta para practicar: no la uses en runs que vayas a mandar a una lista.
