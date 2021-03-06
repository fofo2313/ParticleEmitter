#include "XUSGComputeUtil.h"

using namespace std;
using namespace XUSG;

ComputeUtil::ComputeUtil()
{
}

ComputeUtil::ComputeUtil(const Device& device) :
	m_device(device)
{
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

ComputeUtil::~ComputeUtil()
{
}

bool ComputeUtil::SetPrefixSum(const CommandList& commandList, bool safeMode,
	shared_ptr<DescriptorTableCache> descriptorTableCache, TypedBuffer* pBuffer,
	vector<Resource>* pUploaders, Format format, uint32_t maxElementCount)
{
	if (maxElementCount > 1024 * 1024)
		assert(!"Error: maxElementCount should be no more than 1048576!");
	m_safeMode = safeMode;
	m_descriptorTableCache = descriptorTableCache;
	m_format = format;
	m_maxElementCount = maxElementCount;

	// Create resources
	N_RETURN(m_counter.Create(m_device, 1, sizeof(uint32_t), Format::R32_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::DENY_SHADER_RESOURCE,
		MemoryType::DEFAULT, 0, nullptr, 1, nullptr, L"GlobalBarrierCounter"), false);

	if (pBuffer)
	{
		m_pBuffer = pBuffer;
		// Create a UAV table
		{
			Util::DescriptorTable uavTable;
			uavTable.SetDescriptors(0, 1, &pBuffer->GetUAV());
			X_RETURN(m_uavTables[UAV_TABLE_DATA], uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}

		// Append a counter UAV table
		{
			Util::DescriptorTable uavTable;
			uavTable.SetDescriptors(0, 1, &m_counter.GetUAV());
			X_RETURN(m_uavTables[UAV_TABLE_COUNTER], uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}
	else
	{
		// Select pipeline index and stride
		uint32_t stride = sizeof(uint32_t);
		switch (format)
		{
		case Format::R16_UINT:
		case Format::R16_SINT:
		case Format::R16_FLOAT:
			stride = sizeof(uint16_t);
			break;
		}

		// Create test buffers
		m_testBuffer = make_unique<TypedBuffer>();
		N_RETURN(m_testBuffer->Create(m_device, maxElementCount, stride, format,
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::DENY_SHADER_RESOURCE,
			MemoryType::DEFAULT, 0, nullptr, 1, nullptr, L"PrefixSumTestBuffer"), false);

		m_readBack = make_unique<TypedBuffer>();
		N_RETURN(m_readBack->Create(m_device, maxElementCount, stride, format,
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::READBACK, 0,
			nullptr, 0, nullptr, L"ReadBackBuffer"), false);

		m_pBuffer = m_testBuffer.get();

		// Create a UAV table
		{
			Util::DescriptorTable uavTable;
			uavTable.SetDescriptors(0, 1, &m_testBuffer->GetUAV(), TEMPORARY_POOL);
			X_RETURN(m_uavTables[UAV_TABLE_DATA], uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}

		// Append a counter UAV table
		{
			Util::DescriptorTable uavTable;
			uavTable.SetDescriptors(0, 1, &m_counter.GetUAV(), TEMPORARY_POOL);
			X_RETURN(m_uavTables[UAV_TABLE_COUNTER], uavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}

		// Upload test data
		m_testData.resize(stride * maxElementCount);
		const auto pTestData = m_testData.data();
		switch (format)
		{
		case Format::R32_SINT:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<int32_t*>(pTestData)[i] = (rand() & 1) ? -rand() : rand();
			break;
		case Format::R16_SINT:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<int16_t*>(pTestData)[i] = (rand() & 1) ? -rand() : rand();
			break;
		case Format::R8_SINT:
		case Format::R8_UINT:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<uint8_t*>(pTestData)[i] = rand() & 0xff;
			break;
		case Format::R16_UINT:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<uint16_t*>(pTestData)[i] = rand();
			break;
		case Format::R32_FLOAT:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<float*>(pTestData)[i] = rand() / 1000.0f;
			break;
		default:
			for (auto i = 0u; i < maxElementCount; ++i)
				reinterpret_cast<uint32_t*>(pTestData)[i] = rand();
		}

		if (!pUploaders) assert(!"Error: if pBufferView is nullptr, pUploaders must not be nullptr!");
		pUploaders->emplace_back();
		m_testBuffer->Upload(commandList, pUploaders->back(), m_testData.data(),
			stride * maxElementCount, 0, ResourceState::UNORDERED_ACCESS);
	}

	// Select pipeline index and stride
	auto pipelineIndex = safeMode ? PREFIX_SUM_UINT1 : PREFIX_SUM_UINT;
	switch (format)
	{
	case Format::R32_SINT:
	case Format::R16_SINT:
	case Format::R8_SINT:
		pipelineIndex = safeMode ? PREFIX_SUM_SINT1 : PREFIX_SUM_SINT;
		break;
	case Format::R32_FLOAT:
	case Format::R16_FLOAT:
		pipelineIndex = safeMode ? PREFIX_SUM_FLOAT1 : PREFIX_SUM_FLOAT;
		break;
	}

	// Create pipeline layout
	Util::PipelineLayout pipelineLayout;
	pipelineLayout.SetConstants(0, SizeOfInUint32(uint32_t[2]), 0);
	pipelineLayout.SetRange(1, DescriptorType::UAV, 2, 0);
	X_RETURN(m_pipelineLayouts[pipelineIndex], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
		PipelineLayoutFlag::NONE, L"PrefixSumLayout"), false);

	// Create pipeline
	if (!m_pipelines[pipelineIndex])
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, pipelineIndex,
			safeMode ? L"CSPrefixSum1.cso" : L"CSPrefixSum.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[pipelineIndex]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, pipelineIndex));
		X_RETURN(m_pipelines[pipelineIndex], state.GetPipeline(m_computePipelineCache,
			safeMode ? L"PrefixSum1" : L"PrefixSum"), false);
	}

	if (safeMode)
	{
		// Create pipeline layout
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::UAV, 1, 0);
		X_RETURN(m_pipelineLayouts[pipelineIndex + 1], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"PrefixSum2Layout"), false);

		// Create pipeline
		if (!m_pipelines[pipelineIndex + 1])
		{
			N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, pipelineIndex + 1, L"CSPrefixSum2.cso"), false);

			Compute::State state;
			state.SetPipelineLayout(m_pipelineLayouts[pipelineIndex + 1]);
			state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, pipelineIndex + 1));
			X_RETURN(m_pipelines[pipelineIndex + 1], state.GetPipeline(m_computePipelineCache, L"PrefixSum2"), false);
		}
	}

	return true;
}

void ComputeUtil::SetDevice(const Device& device)
{
	m_device = device;
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

void ComputeUtil::PrefixSum(const CommandList& commandList, uint32_t numElements)
{
	numElements = numElements != UINT32_MAX ? numElements : m_maxElementCount;
	if (m_testBuffer && numElements > m_maxElementCount)
			assert(!"Error: numElements is greater than maxElementCount!");

	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL,
			m_testBuffer ? TEMPORARY_POOL : PERMANENT_POOL),
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Clear counter
	const uint32_t clear[4] = {};
	commandList.ClearUnorderedAccessViewUint(m_uavTables[UAV_TABLE_COUNTER],
		m_counter.GetUAV(), m_counter.GetResource(), clear);

	// Select pipeline index
	auto pipelineIndex = m_safeMode ? PREFIX_SUM_UINT1 : PREFIX_SUM_UINT;
	switch (m_format)
	{
	case Format::R32_SINT:
	case Format::R16_SINT:
	case Format::R8_SINT:
		pipelineIndex = m_safeMode ? PREFIX_SUM_SINT1 : PREFIX_SUM_SINT;
		break;
	case Format::R32_FLOAT:
	case Format::R16_FLOAT:
		pipelineIndex = m_safeMode ? PREFIX_SUM_FLOAT1 : PREFIX_SUM_FLOAT;
		break;
	}

	// Set pipeline state
	commandList.SetComputePipelineLayout(m_pipelineLayouts[pipelineIndex]);
	commandList.SetPipelineState(m_pipelines[pipelineIndex]);

	// Set descriptor tables
	const auto numGroups = DIV_UP(numElements, 1024);
	const auto remainder = numElements & 1023;
	commandList.SetCompute32BitConstant(0, numGroups);
	commandList.SetCompute32BitConstant(0, remainder, 1);
	commandList.SetComputeDescriptorTable(1, m_uavTables[UAV_TABLE_DATA]);

	commandList.Dispatch(numGroups, 1, 1);

	if (m_safeMode)
	{
		// Set barrier
		ResourceBarrier barrier;
		const auto numBarriers = m_pBuffer->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
		commandList.Barrier(numBarriers, &barrier);

		// Set pipeline state
		commandList.SetComputePipelineLayout(m_pipelineLayouts[pipelineIndex + 1]);
		commandList.SetPipelineState(m_pipelines[pipelineIndex + 1]);

		// Set descriptor tables
		commandList.SetComputeDescriptorTable(0, m_uavTables[UAV_TABLE_DATA]);

		commandList.Dispatch(DIV_UP(numElements, 64), 1, 1);
	}

	if (m_readBack)
	{
		assert(m_testBuffer);

		// Set barrier
		ResourceBarrier barrier;
		m_testBuffer->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS); // Promotion
		const auto numBarriers = m_testBuffer->SetBarrier(&barrier, ResourceState::COPY_SOURCE);
		commandList.Barrier(numBarriers, &barrier);

		// Copy the counter for readback
		commandList.CopyResource(m_readBack->GetResource(), m_testBuffer->GetResource());
	}
}

void ComputeUtil::VerifyPrefixSum(uint32_t numElements)
{
	// Generate ground truth
#define GENERATE_GROUND_TRUTH(T) \
	vector<T> groundTruths(numElements); \
	for (size_t i = 1; i < numElements; ++i) \
		groundTruths[i] = groundTruths[i - 1] + reinterpret_cast<const T*>(pTestData)[i - 1]

	// Verify results
#define COMPARE(T) \
	const auto testResults = reinterpret_cast<const T*>(m_readBack->Map(nullptr)); \
	for (size_t i = 0; i < numElements; ++i) \
	{ \
		if (testResults[i] != groundTruths[i]) \
			cout << "Wrong " << i << ": input (" << \
			reinterpret_cast<const T*>(pTestData)[i] << \
			"), result (" << testResults[i] << "), ground truth (" << \
			groundTruths[i] << ")" << endl; \
		else \
			cout << "Correct " << i << ": input (" << \
			reinterpret_cast<const T*>(pTestData)[i] << \
			"), result (" << testResults[i] << "), ground truth (" << \
			groundTruths[i] << ")" << endl; \
		if (i % 1024 == 1023) system("pause"); \
	}

#define VERIFY(T) GENERATE_GROUND_TRUTH(T); COMPARE(T)

	numElements = numElements != UINT32_MAX ? numElements : m_maxElementCount;

	const auto pTestData = m_testData.data();
	switch (m_format)
	{
	case Format::R32_SINT:
	{
		VERIFY(int32_t);
		break;
	}
	case Format::R16_SINT:
	{
		VERIFY(int16_t);
		break;
	}
	case Format::R8_SINT:
	{
		VERIFY(int8_t);
		break;
	}
	case Format::R8_UINT:
	{
		VERIFY(uint8_t);
		break;
	}
	case Format::R16_UINT:
	{
		VERIFY(uint16_t);
		break;
	}
	case Format::R32_FLOAT:
	{
		VERIFY(float);
		break;
	}
	default:
	{
		VERIFY(uint32_t);
	}
	}

	m_readBack->Unmap();

#undef VERIFY
#undef COMPARE
#undef GENERATE_GROUND_TRUTH
}
