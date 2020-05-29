#include "model.h"
#include "window.c"
int main(int argc, char **argv) {
  glfwInit();
  GraphicsState graphics = InitGraphics();
  Model model = loadFromFile("./data/SpaceShipDetailed.obj");
  uint32_t shipDef = CreateEntityDef(&graphics, &model);
  vec3 pos = {0, 0, 0};
  for (uint32_t i = 0; i < 4; i++) {
    pos[0]+= 5;
		pos[2] = 0;
    for (uint32_t k = 0; k < 4; k++) {
      pos[2]+= 5;
      Instance inst = {.scale = {0.01, 0.01, 0.01}};
			memcpy(&inst.position, &pos, sizeof(vec3));
      glm_mat4_identity(inst.rotation);
      AddEntityInstance(&graphics.entities[shipDef], inst);
    }
  }

  while (true) {
    if (glfwWindowShouldClose(graphics.window)) {
      return 0;
    }
    glfwPollEvents();
    MoveCamera(&graphics);
    DrawGraphics(&graphics);
  }
}

