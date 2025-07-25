#pragma once

#include "base_sysfb.h"
#include "engineerrors.h"
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>

struct FRenderViewpoint;
class VkSamplerManager;
class VkBufferManager;
class VkTextureManager;
class VkShaderManager;
class VkCommandBufferManager;
class VkDescriptorSetManager;
class VkRenderPassManager;
class VkFramebufferManager;
class VkLevelMesh;
class VkLightmapper;
class VkLightprober;
class VkRenderState;
class VkStreamBuffer;
class VkHardwareDataBuffer;
class VkHardwareTexture;
class VkRenderBuffers;
class VkPostprocess;
class VkPipelineKey;
class VkRenderPassSetup;
class VkShaderCache;

class VulkanRenderDevice : public SystemBaseFrameBuffer
{
public:
	VulkanRenderDevice(void* hMonitor, bool fullscreen, std::shared_ptr<VulkanInstance> instance, std::shared_ptr<VulkanSurface> surface);
	~VulkanRenderDevice();

	VulkanDevice* GetDevice() { return mDevice.get(); }
	VkShaderCache* GetShaderCache() { return mShaderCache.get(); }
	VkCommandBufferManager* GetCommands() { return mCommands.get(); }
	VkShaderManager *GetShaderManager() { return mShaderManager.get(); }
	VkSamplerManager *GetSamplerManager() { return mSamplerManager.get(); }
	VkBufferManager* GetBufferManager() { return mBufferManager.get(); }
	VkTextureManager* GetTextureManager() { return mTextureManager.get(); }
	VkFramebufferManager* GetFramebufferManager() { return mFramebufferManager.get(); }
	VkDescriptorSetManager* GetDescriptorSetManager() { return mDescriptorSetManager.get(); }
	VkRenderPassManager *GetRenderPassManager() { return mRenderPassManager.get(); }
	VkLevelMesh* GetLevelMesh() { return mLevelMesh.get(); }
	VkLightmapper* GetLightmapper() { return mLightmapper.get(); }
	VkLightprober* GetLightproper() { return mLightprober.get(); }
	VkRenderState *GetRenderState() { return mRenderState.get(); }
	VkPostprocess *GetPostprocess() { return mPostprocess.get(); }
	VkRenderBuffers *GetBuffers() { return mActiveRenderBuffers; }
	FRenderState* RenderState() override;

	bool IsRayQueryEnabled() const override { return mUseRayQuery; }
	bool IsVulkan() override { return true; }

	void Update() override;

	void InitializeState() override;
	bool CompileNextShader() override;
	void PrecacheMaterial(FMaterial *mat, int translation) override;
	void UpdatePalette() override;
	const char* DeviceName() const override;
	int Backend() override { return 1; }
	void SetTextureFilterMode() override;
	void StartPrecaching() override;
	void BeginFrame() override;
	void BlurScene(float amount) override;
	void PostProcessScene(bool swscene, int fixedcm, float flash, bool palettePostprocess, const std::function<void()> &afterBloomDrawEndScene2D) override;
	void UpdateLinearDepthTexture() override;
	void AmbientOccludeScene(float m5) override;
	void SetSceneRenderTarget(bool useSSAO) override;
	void SetLevelMesh(LevelMesh* mesh) override;
	void UpdateLightmaps(const TArray<LightmapTile*>& tiles) override;
	void SetShadowMaps(const TArray<float>& lights, hwrenderer::LevelAABBTree* tree, bool newTree) override;
	void SetSaveBuffers(bool yes) override;
	void ImageTransitionScene(bool unknown) override;
	void SetActiveRenderTarget() override;

	IHardwareTexture *CreateHardwareTexture(int numchannels) override;
	FMaterial* CreateMaterial(FGameTexture* tex, int scaleflags) override;

	IBuffer* CreateVertexBuffer(int numBindingPoints, int numAttributes, size_t stride, const FVertexBufferAttribute* attrs) override;
	IBuffer* CreateIndexBuffer() override;

	FTexture *WipeStartScreen() override;
	FTexture *WipeEndScreen() override;

	TArray<uint8_t> GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma) override;

	bool GetVSync() { return mVSync; }
	void SetVSync(bool vsync) override;

	void Draw2D() override;

	void WaitForCommands(bool finish) override;

	int GetBindlessTextureIndex(FMaterial* material, int clampmode, int translation, bool paletteMode) override;

	int GetLevelMeshPipelineID(const MeshApplyData& applyData, const SurfaceUniforms& surfaceUniforms, const FMaterialState& material) override;
	void DownloadLightmap(int arrayIndex, uint16_t* buffer) override;

	const VkPipelineKey& GetLevelMeshPipelineKey(int id) const;

	bool IsSurfaceAvailable() { return HasSurface; }

	VkFormat DepthStencilFormat = VK_FORMAT_UNDEFINED;
	VkFormat NormalFormat = VK_FORMAT_UNDEFINED;

private:
	void RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc) override;

	void ResetLightProbes() override;
	void RenderLightProbe(int probeIndex, std::function<void(IntRect& bounds, int side)> renderFunc) override;
	void EndLightProbePass() override;
	void DownloadLightProbes(int probeCount, TArrayView<uint16_t> irradianceMaps, TArrayView<uint16_t> prefilterMaps) override;
	void UploadLightProbes(int probeCount, const TArray<uint16_t>& irradianceMaps, const TArray<uint16_t>& prefilterMaps) override;

	void PrintStartupLog();
	void CopyScreenToBuffer(int w, int h, uint8_t *data) override;

	bool SupportsRenderTargetFormat(VkFormat format);
	bool SupportsNormalGBufferFormat(VkFormat format);

	bool HasSurface = false;

	std::shared_ptr<VulkanDevice> mDevice;
	std::unique_ptr<VkShaderCache> mShaderCache;
	std::unique_ptr<VkCommandBufferManager> mCommands;
	std::unique_ptr<VkBufferManager> mBufferManager;
	std::unique_ptr<VkSamplerManager> mSamplerManager;
	std::unique_ptr<VkTextureManager> mTextureManager;
	std::unique_ptr<VkFramebufferManager> mFramebufferManager;
	std::unique_ptr<VkShaderManager> mShaderManager;
	std::unique_ptr<VkRenderBuffers> mScreenBuffers;
	std::unique_ptr<VkRenderBuffers> mSaveBuffers;
	std::unique_ptr<VkPostprocess> mPostprocess;
	std::unique_ptr<VkDescriptorSetManager> mDescriptorSetManager;
	std::unique_ptr<VkRenderPassManager> mRenderPassManager;
	std::unique_ptr<VkLevelMesh> mLevelMesh;
	std::unique_ptr<VkLightmapper> mLightmapper;
	std::unique_ptr<VkLightprober> mLightprober;
	std::unique_ptr<VkRenderState> mRenderState;

	VkRenderBuffers *mActiveRenderBuffers = nullptr;

	bool mVSync = false;
	bool mUseRayQuery = false;

	LevelMesh* levelMesh = nullptr;
	bool levelMeshChanged = true;
	std::unique_ptr<LevelMesh> NullMesh;

	int levelVertexFormatIndex = -1;
	TArray<VkPipelineKey> levelMeshPipelineKeys;
};

class CVulkanError : public CEngineError
{
public:
	CVulkanError() : CEngineError() {}
	CVulkanError(const char* message) : CEngineError(message) {}
};
