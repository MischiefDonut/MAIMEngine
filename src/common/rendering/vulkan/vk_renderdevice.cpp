/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <zvulkan/vulkanobjects.h>

#include <inttypes.h>

#include "v_video.h"
#include "m_png.h"

#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"

#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "flatvertices.h"

#include "vk_renderdevice.h"
#include "vulkan/vk_renderstate.h"
#include "vulkan/vk_postprocess.h"
#include "vulkan/accelstructs/vk_raytrace.h"
#include "vulkan/accelstructs/vk_lightmap.h"
#include "vulkan/pipelines/vk_renderpass.h"
#include "vulkan/descriptorsets/vk_descriptorset.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/samplers/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/framebuffers/vk_framebuffer.h"
#include "vulkan/commands/vk_commandbuffer.h"
#include "vulkan/buffers/vk_hwbuffer.h"
#include "vulkan/buffers/vk_buffer.h"
#include "vulkan/buffers/vk_rsbuffers.h"
#include <zvulkan/vulkanswapchain.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "engineerrors.h"
#include "c_dispatch.h"
#include "menu.h"
#include "cmdlib.h"

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(Bool, vk_rayquery, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void I_BuildVKDeviceList(FOptionValues* opt)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		unsigned int idx = opt->mValues.Reserve(1);
		opt->mValues[idx].Value = (double)i;
		opt->mValues[idx].Text = SupportedDevices[i].Device->Properties.Properties.deviceName;
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) : SystemBaseFrameBuffer(hMonitor, fullscreen)
{
	VulkanDeviceBuilder builder;
	if (vk_rayquery)
		builder.OptionalRayQuery();
	builder.RequireExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	SupportedDevices = builder.FindDevices(surface->Instance);
	mDevice = builder.Create(surface->Instance);

	bool supportsBindless =
		mDevice->EnabledFeatures.DescriptorIndexing.descriptorBindingPartiallyBound &&
		mDevice->EnabledFeatures.DescriptorIndexing.runtimeDescriptorArray &&
		mDevice->EnabledFeatures.DescriptorIndexing.shaderSampledImageArrayNonUniformIndexing;
	if (!supportsBindless)
	{
		I_FatalError("This GPU does not support the minimum requirements of this application");
	}
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	vkDeviceWaitIdle(mDevice->device); // make sure the GPU is no longer using any objects before RAII tears them down

	delete mSkyData;
	delete mShadowMap;

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (mDevice->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	uniformblockalignment = (unsigned int)mDevice->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = mDevice->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));
	mLightmap.reset(new VkLightmap(this));

	mBufferManager->Init();

	mSkyData = new FSkyVertexBuffer(this);
	mShadowMap = new ShadowMap(this);

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();

#ifdef __APPLE__
	mRenderState = std::make_unique<VkRenderStateMolten>(this);
#else
	mRenderState = std::make_unique<VkRenderState>(this);
#endif
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	GetPostprocess()->SetActiveRenderTarget();

	Draw2D();
	twod->Clear();

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	mCommands->WaitForCommands(true);
	mCommands->UpdateGpuStats();

	SystemBaseFrameBuffer::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->View.get(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());

	tex->SetUpdated(true);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
}

const char* VulkanRenderDevice::DeviceName() const
{
	const auto &props = mDevice->PhysicalDevice.Properties;
	return props.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IBuffer*VulkanRenderDevice::CreateVertexBuffer(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs)
{
	return GetBufferManager()->CreateVertexBuffer(numBindingPoints, numAttributes, stride, attrs);
}

IBuffer*VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(mDevice.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(mDevice.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	if (levelMeshChanged)
	{
		levelMeshChanged = false;
		mRaytrace->SetLevelMesh(levelMesh);

		if (levelMesh && levelMesh->StaticMesh->GetSurfaceCount() > 0)
		{
			GetTextureManager()->CreateLightmap(levelMesh->StaticMesh->LMTextureSize, levelMesh->StaticMesh->LMTextureCount, std::move(levelMesh->StaticMesh->LMTextureData));
			GetLightmap()->SetLevelMesh(levelMesh);
		}
	}

	SetViewportRects(nullptr);
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	mScreenBuffers->BeginFrame(screen->mScreenViewport.width, screen->mScreenViewport.height, screen->mSceneViewport.width, screen->mSceneViewport.height);
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
	mRaytrace->BeginFrame();
	mLightmap->BeginFrame();
}

void VulkanRenderDevice::Draw2D()
{
	::Draw2D(twod, *RenderState());
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = mDevice->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : mDevice->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(LevelMesh* mesh)
{
	levelMesh = mesh;
	levelMeshChanged = true;
}

void VulkanRenderDevice::UpdateLightmaps(const TArray<LevelMeshSurface*>& surfaces)
{
	GetLightmap()->Raytrace(surfaces);
}

void VulkanRenderDevice::SetShadowMaps(const TArray<float>& lights, hwrenderer::LevelAABBTree* tree, bool newTree)
{
	auto buffers = GetBufferManager();

	buffers->Shadowmap.Lights->SetData(sizeof(float) * lights.Size(), lights.Data(), BufferUsageType::Stream);

	if (newTree)
	{
		buffers->Shadowmap.Nodes->SetData(tree->NodesSize(), tree->Nodes(), BufferUsageType::Static);
		buffers->Shadowmap.Lines->SetData(tree->LinesSize(), tree->Lines(), BufferUsageType::Static);
	}
	else if (tree->Update())
	{
		buffers->Shadowmap.Nodes->SetSubData(tree->DynamicNodesOffset(), tree->DynamicNodesSize(), tree->DynamicNodes());
		buffers->Shadowmap.Lines->SetSubData(tree->DynamicLinesOffset(), tree->DynamicLinesSize(), tree->DynamicLines());
	}

	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.View.get(), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples());
}

int VulkanRenderDevice::GetBindlessTextureIndex(FMaterial* material, int clampmode, int translation)
{
	FMaterialState materialState;
	materialState.mMaterial = material;
	materialState.mClampMode = clampmode;
	materialState.mTranslation = translation;
	return static_cast<VkMaterial*>(material)->GetBindlessIndex(materialState);
}

void VulkanRenderDevice::DrawLevelMesh(const HWViewpointUniforms& viewpoint)
{
	auto cmdbuffer = GetCommands()->GetDrawCommands();
	auto buffers = GetBuffers();
	auto descriptors = GetDescriptorSetManager();

	VkRenderPassKey key = {};
	key.DrawBufferFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	key.Samples = buffers->GetSceneSamples();
	key.DrawBuffers = 1; // 3 if ssao is enabled
	key.DepthStencil = true;

	auto passSetup = GetRenderPassManager()->GetRenderPass(key);

	RenderPassBegin beginInfo;
	beginInfo.RenderPass(passSetup->GetRenderPass(CT_Color | CT_Depth | CT_Stencil));
	beginInfo.RenderArea(0, 0, buffers->GetWidth(), buffers->GetHeight());
	beginInfo.Framebuffer(buffers->GetFramebuffer(key));
	beginInfo.AddClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
	if (key.DrawBuffers > 1)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (key.DrawBuffers > 2)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	beginInfo.AddClearDepthStencil(1.0f, 0);
	beginInfo.Execute(cmdbuffer);

	VkViewport viewport = {};
	viewport.x = (float)mSceneViewport.left;
	viewport.y = (float)mSceneViewport.top;
	viewport.width = (float)mSceneViewport.width;
	viewport.height = (float)mSceneViewport.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	cmdbuffer->setViewport(0, 1, &viewport);

	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = mSceneViewport.width;
	scissor.extent.height = mSceneViewport.height;
	cmdbuffer->setScissor(0, 1, &scissor);

	cmdbuffer->setStencilReference(VK_STENCIL_FRONT_AND_BACK, 0);
	cmdbuffer->setDepthBias(0.0f, 0.0f, 0.0f);

	static const FVertexBufferAttribute format[] =
	{
		{ 0, VATTR_VERTEX, VFmt_Float4, (int)myoffsetof(SurfaceVertex, pos.X) },
		{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(SurfaceVertex, uv.X) },
		{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(SurfaceVertex, lightmap.X) },
	};
	int vertexFormatIndex = GetRenderPassManager()->GetVertexFormat(1, 3, sizeof(SurfaceVertex), format);
	VkBuffer vertexBuffers[2] = { GetRaytrace()->GetVertexBuffer()->buffer, GetRaytrace()->GetVertexBuffer()->buffer };
	VkDeviceSize vertexBufferOffsets[] = { 0, 0 };
	cmdbuffer->bindVertexBuffers(0, 2, vertexBuffers, vertexBufferOffsets);
	cmdbuffer->bindIndexBuffer(GetRaytrace()->GetIndexBuffer()->buffer, 0, VK_INDEX_TYPE_UINT32);

	VkPipelineKey pipelineKey;
	pipelineKey.DrawType = DT_Triangles;
	pipelineKey.VertexFormat = vertexFormatIndex;
	pipelineKey.RenderStyle = DefaultRenderStyle();
	pipelineKey.DepthTest = true;
	pipelineKey.DepthWrite = true;
	pipelineKey.DepthFunc = DF_Less;
	pipelineKey.DepthClamp = false;
	pipelineKey.DepthBias = false;
	pipelineKey.StencilTest = false;
	pipelineKey.StencilPassOp = 0;
	pipelineKey.ColorMask = 15;
	pipelineKey.CullMode = 0;
	pipelineKey.NumTextureLayers = 0;
	pipelineKey.NumTextureLayers = max(pipelineKey.NumTextureLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS);// Always force minimum 8 textures as the shader requires it
	pipelineKey.ShaderKey.SpecialEffect = EFF_NONE;
	pipelineKey.ShaderKey.EffectState = SHADER_NoTexture;
	pipelineKey.ShaderKey.AlphaTest = false;
	pipelineKey.ShaderKey.SWLightRadial = true;
	pipelineKey.ShaderKey.LightMode = 1; // Software
	pipelineKey.ShaderKey.UseShadowmap = gl_light_shadows == 1;
	pipelineKey.ShaderKey.UseRaytrace = gl_light_shadows == 2;
	pipelineKey.ShaderKey.GBufferPass = key.DrawBuffers > 1;
	pipelineKey.ShaderKey.UseLevelMesh = true;

	VulkanPipelineLayout* layout = GetRenderPassManager()->GetPipelineLayout(pipelineKey.NumTextureLayers);

	cmdbuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, passSetup->GetPipeline(pipelineKey));

	auto rsbuffers = GetBufferManager()->GetRSBuffers();
	memcpy(((char*)rsbuffers->Viewpoint.Data) + rsbuffers->Viewpoint.UploadIndex * rsbuffers->Viewpoint.BlockAlign, &viewpoint, sizeof(HWViewpointUniforms));
	int viewpointIndex = rsbuffers->Viewpoint.UploadIndex++;

	StreamData streamdata = {};
	streamdata.uFogColor = 0xffffffff;
	streamdata.uDesaturationFactor = 0.0f;
	streamdata.uAlphaThreshold = 0.5f;
	streamdata.uAddColor = 0;
	streamdata.uObjectColor = 0xffffffff;
	streamdata.uObjectColor2 = 0;
	streamdata.uTextureBlendColor = 0;
	streamdata.uTextureAddColor = 0;
	streamdata.uTextureModulateColor = 0;
	streamdata.uLightDist = 0.0f;
	streamdata.uLightFactor = 0.0f;
	streamdata.uFogDensity = 0.0f;
	streamdata.uLightLevel = 255.0f;// -1.0f;
	streamdata.uInterpolationFactor = 0;
	streamdata.uVertexColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	streamdata.uGlowTopColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uGlowBottomColor = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uGlowTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uGlowBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uGradientTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uGradientBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uSplitTopPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uSplitBottomPlane = { 0.0f, 0.0f, 0.0f, 0.0f };
	streamdata.uDynLightColor = { 0.0f, 0.0f, 0.0f, 1.0f };
	streamdata.uDetailParms = { 0.0f, 0.0f, 0.0f, 0.0f };
#ifdef NPOT_EMULATION
	streamdata.uNpotEmulation = { 0,0,0,0 };
#endif
	streamdata.uClipSplit.X = -1000000.f;
	streamdata.uClipSplit.Y = 1000000.f;

	rsbuffers->StreamBuffer->Write(streamdata);

	MatricesUBO matrices = {};
	matrices.ModelMatrix.loadIdentity();
	matrices.NormalModelMatrix.loadIdentity();
	matrices.TextureMatrix.loadIdentity();
	rsbuffers->MatrixBuffer->Write(matrices);

	uint32_t viewpointOffset = viewpointIndex * rsbuffers->Viewpoint.BlockAlign;
	uint32_t matrixOffset = rsbuffers->MatrixBuffer->Offset();
	uint32_t streamDataOffset = rsbuffers->StreamBuffer->Offset();
	uint32_t lightsOffset = 0;
	uint32_t offsets[4] = { viewpointOffset, matrixOffset, streamDataOffset, lightsOffset };
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptors->GetFixedDescriptorSet());
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, descriptors->GetRSBufferDescriptorSet(), 4, offsets);
	cmdbuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, descriptors->GetNullTextureDescriptorSet());

	PushConstants pushConstants = {};
	pushConstants.uDataIndex = rsbuffers->StreamBuffer->DataIndex();
	pushConstants.uLightIndex = -1;
	pushConstants.uBoneIndexBase = -1;
	cmdbuffer->pushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants), &pushConstants);

	cmdbuffer->drawIndexed(GetRaytrace()->GetIndexCount(), 1, 0, 0, 0);

	cmdbuffer->endRenderPass();
}
