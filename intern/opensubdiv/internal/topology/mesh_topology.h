/* SPDX-FileCopyrightText: 2020 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Author: Sergey Sharybin. */

#ifndef OPENSUBDIV_MESH_TOPOLOGY_H_
#define OPENSUBDIV_MESH_TOPOLOGY_H_

#include <vector>

#include "internal/base/memory.h"

struct OpenSubdiv_Converter;

namespace blender::opensubdiv {

// Simplified representation of mesh topology.
// Only includes parts of actual mesh topology which is needed to perform
// comparison between Application side and OpenSubddiv side.
//
// NOTE: It is an optimized storage which requires special order of topology
// specification. Basically, counters is to be set prior to anything else, in
// the following manner:
//
//   MeshTopology mesh_topology;
//
//   mesh_topology.setNumVertices(...);
//   mesh_topology.setNumEdges(...);
//   mesh_topology.setNumFaces(...);
//
//   for (...) {
//     mesh_topology.setNumFaceVertices(...);
//   }
//
//   mesh_topology.finishResizeTopology();
//
//   /* it is now possible to set vertices of edge, vertices of face, and
//    * sharpness. */
class MeshTopology {
 public:
  MeshTopology();
  MeshTopology(const MeshTopology &other) = default;
  MeshTopology(MeshTopology &&other) noexcept = default;
  ~MeshTopology();

  MeshTopology &operator=(const MeshTopology &other) = default;
  MeshTopology &operator=(MeshTopology &&other) = default;

  //////////////////////////////////////////////////////////////////////////////
  // Vertices.

  void setNumVertices(int num_vertices);
  int getNumVertices() const;

  void setVertexSharpness(int vertex_index, float sharpness);
  float getVertexSharpness(int vertex_index) const;

  //////////////////////////////////////////////////////////////////////////////
  // Edges.

  void setNumEdges(int num_edges);

  // NOTE: Unless full topology was specified will return number of edges based
  // on last edge index for which topology tag was specified.
  int getNumEdges() const;

  void setEdgeVertexIndices(int edge_index, int v1, int v2);
  void getEdgeVertexIndices(int edge_index, int *v1, int *v2) const;

  bool isEdgeEqual(int edge_index, int expected_v1, int expected_v2) const;

  void setEdgeSharpness(int edge_index, float sharpness);
  float getEdgeSharpness(int edge_index) const;

  //////////////////////////////////////////////////////////////////////////////
  // Faces.

  void setNumFaces(int num_faces);

  int getNumFaces() const;

  void setNumFaceVertices(int face_index, int num_face_vertices);
  int getNumFaceVertices(int face_index) const;

  void setFaceVertexIndices(int face_index,
                            int num_face_vertex_indices,
                            const int *face_vertex_indices);

  bool isFaceVertexIndicesEqual(int face_index,
                                int num_expected_face_vertex_indices,
                                const int *expected_face_vertex_indices) const;
  bool isFaceVertexIndicesEqual(int face_index,
                                const std::vector<int> &expected_face_vertex_indices) const;

  //////////////////////////////////////////////////////////////////////////////
  // Pipeline related.

  // This function is to be called when number of vertices, edges, faces, and
  // face-vertices are known.
  //
  // Usually is called from the end of topology refiner factory's
  // resizeComponentTopology().
  void finishResizeTopology();

  //////////////////////////////////////////////////////////////////////////////
  // Comparison.

  // Compare given topology with converter. Returns truth if topology
  // matches given converter, false otherwise.
  //
  // This allows users to construct converter (which is supposed to be cheap)
  // and compare with existing topology before going into more computationally
  // complicated parts of subdivision process.
  bool isEqualToConverter(const OpenSubdiv_Converter *converter) const;

 protected:
  // Edges are allowed to be stored sparsly, to save memory used by
  // non-semi-sharp edges.
  void ensureNumEdgesAtLeast(int num_edges);

  // Geometry tags are stored sparsly.
  //
  // These functions ensures that the storage can be addressed by an index which
  // corresponds to the given size.
  void ensureVertexTagsSize(int num_vertices);
  void ensureEdgeTagsSize(int num_edges);

  // Get pointer to the memory where face vertex indices are stored.
  int *getFaceVertexIndicesStorage(int face_index);
  const int *getFaceVertexIndicesStorage(int face_index) const;

  struct VertexTag {
    float sharpness = 0.0f;
  };

  struct Edge {
    int v1 = -1;
    int v2 = -1;
  };

  struct EdgeTag {
    float sharpness = 0.0f;
  };

  int num_vertices_;
  std::vector<VertexTag> vertex_tags_;

  int num_edges_;
  std::vector<Edge> edges_;
  std::vector<EdgeTag> edge_tags_;

  int num_faces_;

  // Continuous array of all vertices of all faces:
  //  [vertex indices of face 0][vertex indices of face 1] .. [vertex indices of face n].
  std::vector<int> face_vertex_indices_;

  // Indexed by face contains index within face_vertex_indices_ which corresponds
  // to the element which contains first vertex of the face.
  std::vector<int> faces_first_vertex_index_;

  MEM_CXX_CLASS_ALLOC_FUNCS("MeshTopology");
};

}  // namespace blender::opensubdiv

#endif  // OPENSUBDIV_MESH_TOPOLOGY_H_
