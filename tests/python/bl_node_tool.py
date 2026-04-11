# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import unittest
import bpy


def create_join_geometry_tool(tool_idname):
    """
    Create a geometry node group set up as an object-mode tool.
    Returns the node tree and the identifier of the Object input socket.
    """
    tree = bpy.data.node_groups.new("TestNodeTool", "GeometryNodeTree")
    tree.is_tool = True
    tree.is_mode_object = True
    tree.is_type_mesh = True
    tree.node_tool_idname = tool_idname

    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    obj_socket = tree.interface.new_socket("Object", in_out='INPUT', socket_type='NodeSocketObject')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')

    group_input = tree.nodes.new("NodeGroupInput")
    group_output = tree.nodes.new("NodeGroupOutput")
    obj_info = tree.nodes.new("GeometryNodeObjectInfo")
    join_geo = tree.nodes.new("GeometryNodeJoinGeometry")

    tree.links.new(group_input.outputs["Geometry"], join_geo.inputs["Geometry"])
    tree.links.new(group_input.outputs["Object"], obj_info.inputs["Object"])
    tree.links.new(obj_info.outputs["Geometry"], join_geo.inputs["Geometry"])
    tree.links.new(join_geo.outputs["Geometry"], group_output.inputs["Geometry"])

    return tree, obj_socket.identifier


class TestNodeTool(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_join_geometry_with_object_input(self):
        from bpy.types import WindowManager
        # Add a cube (the active object the tool will run on).
        bpy.ops.mesh.primitive_cube_add()
        cube = bpy.context.active_object
        cube_vertex_count = len(cube.data.vertices)

        # Add a monkey/Suzanne (the tool's object input, joined into the cube).
        bpy.ops.mesh.primitive_monkey_add()
        monkey = bpy.context.active_object
        monkey_vertex_count = len(monkey.data.vertices)

        # Make the cube the active, selected object.
        bpy.context.view_layer.objects.active = cube
        for obj in bpy.context.scene.objects:
            obj.select_set(obj == cube)

        _tree, obj_input_identifier = create_join_geometry_tool("geometry.test_node_tool_join_geometry")

        WindowManager.register_node_group_operators()

        bpy.ops.geometry.test_node_tool_join_geometry(
            'EXEC_DEFAULT',
            inputs={obj_input_identifier: {"value": monkey.name}},
        )

        # The cube's mesh should now contain geometry from both the cube and the
        # monkey, so the vertex count should be the sum of both.
        expected_vertex_count = cube_vertex_count + monkey_vertex_count
        self.assertEqual(len(cube.data.vertices), expected_vertex_count)


if __name__ == "__main__":
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
