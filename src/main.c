#include "model.h"
#include "window.c"
int main(int argc, char **argv) {
	glfwInit();
  GraphicsState graphics = InitGraphics();
  Model model = loadFromFile("./data/SpaceShipDetailed.obj");
  uint32_t shipDef = CreateEntityDef(&graphics, &model);
  Instance inst = {.position = {0, 0, 0}, .scale = {0.01, 0.01, 0.01}};
	glm_mat4_identity(inst.rotation);
  AddEntityInstance(&graphics.entities[shipDef], inst);

  while (true) {
		if(glfwWindowShouldClose(graphics.window)) {
			return 0;
		}
    glfwPollEvents();
		MoveCamera(&graphics);
    DrawGraphics(&graphics);
  }
}

