#ifndef OPENDOM_MODEL
#define OPENDOM_MODEL
#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cglm/cglm.h>
#include <vulkan/vulkan.h>
typedef struct Vertex {
  vec4 position;
  vec4 color;
  vec4 normal;
} Vertex;

typedef struct Model {
  Vertex *vertices;
  uint32_t vertexCount;
  char *materialPath;
  VkBuffer vertexBuffer;
  VkDeviceMemory vertexMemory;
} Model;

typedef struct Instance {
  mat4 rotation;
  vec4 position;
	vec4 scale;
	uint32_t instanceId;
	bool selected;
} Instance;

typedef struct EntityDef {
  Model model;
  bool safeToUpdate;
  Instance *instances;
  VkBuffer instanceBuffer;
  uint32_t maxInstances;
  VkDeviceMemory instanceMemory;
  uint32_t instanceCount;
  bool *dirtyBuffer;
} EntityDef;

Model loadFromFile(char *path) {
  Model model = {.vertexCount = 0, .vertices = 0};
  const struct aiScene *scene =
      aiImportFile(path, aiProcess_CalcTangentSpace | aiProcess_Triangulate |
                             aiProcess_JoinIdenticalVertices |
                             aiProcess_PreTransformVertices |
                             aiProcess_SortByPType | aiProcess_OptimizeGraph);
  uint32_t t;
  for (t = 0; t < scene->mNumMeshes; t++) {
    model.vertexCount += scene->mMeshes[t]->mNumFaces * 3;
  }
  model.vertices = calloc(model.vertexCount, sizeof(Vertex));
	printf("%d vertices\n", model.vertexCount);
  Vertex *vertices = model.vertices;
  for (t = 0; t < scene->mNumMeshes; t++) {
    struct aiMesh *mesh = scene->mMeshes[t];
    uint32_t i = 0;
    for (i = 0; i < mesh->mNumFaces; i++) {
      struct aiFace face = mesh->mFaces[i];
      uint32_t k = 0;
      for (k = 0; k < face.mNumIndices; k++) {
        uint32_t index = face.mIndices[k];
        vertices[i * 3 + k] = (Vertex){
            .color = {1, 1, 1, 1},
            .position = {mesh->mVertices[index].x, mesh->mVertices[index].y,
                         mesh->mVertices[index].z, 1},
            .normal = {mesh->mNormals[index].x, mesh->mNormals[index].y,
                       mesh->mNormals[index].z, 1}};
      }
    }
		printf("Next!\n");
    vertices = &vertices[mesh->mNumFaces * 3];
  }
  printf("Generated\n");
  aiReleaseImport(scene);
  return model;
}

#endif
