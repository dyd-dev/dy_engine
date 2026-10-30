"""Independent Blender reference for the imported linear COLOR_0 fixture.

Run with an official Blender background process and pass --input/--output after
its `--` separator. This changes only the in-memory reference scene. The explicit
Reinhard output node matches the engine test's output transform; lighting, shadows
and tone mapping are deliberately not being compared by this unlit fixture.
"""
import argparse
import json
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(Path(args.input).resolve()))
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    assert meshes, "Reference asset contains no geometry"
    corners = [o.matrix_world @ Vector(v) for o in meshes for v in o.bound_box]
    low = Vector([min(v[i] for v in corners) for i in range(3)])
    high = Vector([max(v[i] for v in corners) for i in range(3)])
    center, scale = (low + high) * 0.5, 1.4 / max(high - low)
    for obj in meshes:
        world = obj.matrix_world.copy()
        for vertex in obj.data.vertices:
            vertex.co = ((world @ vertex.co) - center) * scale
        obj.matrix_world.identity()
        material = bpy.data.materials.new("linear vertex color / explicit Reinhard")
        material.use_nodes = True
        nodes, links = material.node_tree.nodes, material.node_tree.links
        nodes.clear()
        color = nodes.new("ShaderNodeVertexColor")
        color.layer_name = obj.data.color_attributes[0].name
        denominator = nodes.new("ShaderNodeVectorMath")
        denominator.operation = "ADD"
        denominator.inputs[1].default_value = (1, 1, 1)
        links.new(color.outputs["Color"], denominator.inputs[0])
        mapped = nodes.new("ShaderNodeVectorMath")
        mapped.operation = "DIVIDE"
        links.new(color.outputs["Color"], mapped.inputs[0])
        links.new(denominator.outputs["Vector"], mapped.inputs[1])
        emission = nodes.new("ShaderNodeEmission")
        links.new(mapped.outputs["Vector"], emission.inputs["Color"])
        output = nodes.new("ShaderNodeOutputMaterial")
        links.new(emission.outputs[0], output.inputs["Surface"])
        obj.data.materials.clear()
        obj.data.materials.append(material)
        for polygon in obj.data.polygons:
            polygon.material_index = 0

    scene = bpy.context.scene
    camera = bpy.data.objects.new("reference camera", bpy.data.cameras.new("reference camera"))
    scene.collection.objects.link(camera)
    camera.location = (0, -2, 0)
    camera.rotation_euler = Vector((0, 1, 0)).to_track_quat("-Z", "Y").to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 2
    camera.data.clip_start, camera.data.clip_end = 0.1, 10
    scene.camera = camera
    world = bpy.data.worlds.new("display clear color")
    world.use_nodes = True
    def linear(c):
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (
        linear(0.07), linear(0.11), linear(0.19), 1)
    scene.world = world
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 16
    scene.cycles.use_denoising = False
    scene.render.resolution_x = scene.render.resolution_y = 256
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.dither_intensity = 0
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure, scene.view_settings.gamma = 0, 1
    scene.render.filepath = str(Path(args.output).resolve())
    bpy.ops.render.render(write_still=True)
    print("REFERENCE_RESULT=" + json.dumps({
        "blender": bpy.app.version_string, "input": str(Path(args.input).resolve()),
        "objects": len(meshes), "triangles": sum(len(p.vertices)-2 for o in meshes for p in o.data.polygons),
        "output": scene.render.filepath, "contract": "unlit COLOR_0, explicit Reinhard, sRGB",
    }), flush=True)


if __name__ == "__main__":
    main()
