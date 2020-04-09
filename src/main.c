#include "model.h"
#include "window.c"
int main(int argc, char **argv) {
  GraphicsState graphics = InitGraphics();
  Model model = loadFromFile("./data/SpaceShipDetailed.obj");
  uint32_t shipDef = CreateEntityDef(&graphics, &model);
  Instance inst = {.position = {0, 0, 0}, .scale = {1, 1, 1}};
	glm_mat4_identity(inst.rotation);
  AddEntityInstance(&graphics.entities[shipDef], inst);

  while (true) {
    glfwPollEvents();
		MoveCamera(&graphics);
    DrawGraphics(&graphics);
  }
}

