#ifndef INCLUDED_VKBENCH_H
#define INCLUDED_VKBENCH_H

// Vulkan vs OpenGL ES feasibility test for the Quest build (see VkBench.cpp). Started by putting
// "vkbench" in Config/StartGame.txt; the result is written to Log/VkBench.txt.
#ifdef __ANDROID__
#include <string>
namespace VkBench
{
// Runs the whole test (about 30 seconds) and returns the report text. Also saved to outPath.
std::string Run(const std::string& outPath);
}
#endif
#endif
