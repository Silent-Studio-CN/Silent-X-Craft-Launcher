---- Minecraft Crash Report ----
// Oh dear.

Time: 2026-01-01 00:00:00
Description: Rendering overlay

java.lang.OutOfMemoryError: Java heap space
	at net.minecraft.client.renderer.LevelRenderer.render(LevelRenderer.java:123)

A detailed walkthrough of the error, its code path and all known details is as follows:
---------------------------------------------------------------------------------------

-- System Details --
Details:
	Minecraft Version: 1.0
	Java Version: 26.0.2, Oracle Corporation
Caused by: java.lang.OutOfMemoryError: Java heap space
	at com.example.mod.Main.tick(Main.java:42)
