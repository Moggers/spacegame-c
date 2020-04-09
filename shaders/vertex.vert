#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNorm;
layout(location = 3) in mat4 instanceRotation;
layout(location = 7) in vec3 instancePosition;
layout(location = 8) in vec3 instanceScale;
layout(location = 9) in uint instanceId;

layout(set = 0, binding = 0) uniform Block {
	mat4 model;
	mat4 view;
  mat4 proj;
};

layout(set = 0, binding = 1) buffer Block {
  vec4 mouse;
	vec2 windowSize;
  uint mouseButtons;
	uint selectionBufferLength;
	uint frameId;
  uint selectionMap[4096];
  uint selectionBuffer[256];
};

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec3 fragNorm;
layout(location = 2) out flat uint outInstanceId;
layout(location = 3) out mat4 mvp;
layout(location = 7) out vec4 preproj;

void main() {
		if((mouseButtons & 2) == 2) {
			if(selectionMap[instanceId] != 0 && 
				!(selectionMap[instanceId] == 15 && frameId == 1) &&
				!(selectionMap[instanceId] == 15 && frameId == 2) &&
				!(selectionMap[instanceId] == (frameId - 0)) &&
			  !(selectionMap[instanceId] == (frameId - 1)) &&
				!(selectionMap[instanceId] == (frameId - 2))
				 ) {
				selectionMap[instanceId] = 0;
			}
		}
    gl_Position = (proj * inverse(view)) * (instanceRotation * vec4(inPosition * instanceScale,1) + vec4(instancePosition,1));
		mvp = proj * view;
		preproj = (instanceRotation * vec4(inPosition * instanceScale,1) + vec4(instancePosition,1));
    fragColor = inColor;
		fragNorm = vec3(instanceRotation * vec4(inNorm, 1));
		outInstanceId = instanceId;
}
