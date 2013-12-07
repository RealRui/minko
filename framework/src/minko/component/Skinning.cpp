/*
Copyright (c) 2013 Aerys

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "minko/component/Skinning.hpp"

#include <minko/scene/Node.hpp>
#include <minko/scene/NodeSet.hpp>
#include <minko/data/ArrayProvider.hpp>
#include <minko/geometry/Geometry.hpp>
#include <minko/geometry/Bone.hpp>
#include <minko/geometry/Skin.hpp>
#include <minko/render/AbstractContext.hpp>
#include <minko/math/Matrix4x4.hpp>
#include <minko/component/Surface.hpp>
#include <minko/component/SceneManager.hpp>

using namespace minko;
using namespace minko::data;
using namespace minko::scene;
using namespace minko::math;
using namespace minko::component;
using namespace minko::geometry;
using namespace minko::render;

/*static*/ const unsigned int	Skinning::MAX_NUM_BONES_PER_VERTEX	= 8;
/*static*/ const std::string	Skinning::PNAME_NUM_BONES			= "geometry.numBones";
/*static*/ const std::string	Skinning::PNAME_BONE_MATRICES		= "geometry.boneMatrices";
/*static*/ const std::string	Skinning::ATTRNAME_POSITION			= "position";
/*static*/ const std::string	Skinning::ATTRNAME_NORMAL			= "normal";
/*static*/ const std::string	Skinning::ATTRNAME_BONE_IDS_A		= "boneIdsA";
/*static*/ const std::string	Skinning::ATTRNAME_BONE_IDS_B		= "boneIdsB";
/*static*/ const std::string	Skinning::ATTRNAME_BONE_WEIGHTS_A	= "boneWeightsA";
/*static*/ const std::string	Skinning::ATTRNAME_BONE_WEIGHTS_B	= "boneWeightsB";

Skinning::Skinning(const Skin::Ptr		skin, 
				   SkinningMethod		method,
				   AbstractContext::Ptr	context):
	AbstractComponent(),
	_skin(skin),
	_context(context),
	_method(method),
	_boneVertexBuffer(nullptr),
	_targetGeometry(),
	_targetStartTime(),
	_targetInputPositions(),
	_targetInputNormals(),
	_targetAddedSlot(nullptr),
	_targetRemovedSlot(nullptr),
	_addedSlot(nullptr),
	_removedSlot(nullptr),
	_frameBeginSlot(nullptr)
{
}

void
Skinning::initialize()
{
	if (_skin == nullptr)
		throw std::invalid_argument("skin");

	if (_context == nullptr)
		throw std::invalid_argument("context");

	if (_method != SkinningMethod::SOFTWARE && _skin->maxNumVertexBones() > MAX_NUM_BONES_PER_VERTEX)
	{
		std::cerr << "The maximum number of bones per vertex gets too high (" << _skin->maxNumVertexBones() 
			<< ") to propose hardware skinning (max allowed = " << MAX_NUM_BONES_PER_VERTEX << ")" 
			<< std::endl;

		_method	= SkinningMethod::SOFTWARE;
	}

	_boneVertexBuffer	= _method == SkinningMethod::SOFTWARE 
		? nullptr 
		: createVertexBufferForBones();

	_targetAddedSlot = targetAdded()->connect(std::bind(
		&Skinning::targetAddedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2
	));

	_targetRemovedSlot = targetRemoved()->connect(std::bind(
		&Skinning::targetRemovedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2
	));
}

void
Skinning::targetAddedHandler(AbstractComponent::Ptr, Node::Ptr target)
{
	_addedSlot	= target->added()->connect(std::bind(
		&Skinning::addedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2,
		std::placeholders::_3
	));

	_removedSlot	= target->removed()->connect(std::bind(
		&Skinning::removedHandler,
		shared_from_this(),
		std::placeholders::_1,
		std::placeholders::_2,
		std::placeholders::_3
	));
}

void
Skinning::targetRemovedHandler(AbstractComponent::Ptr, Node::Ptr)
{
	_addedSlot		= nullptr;
	_removedSlot	= nullptr;
}

void
Skinning::addedHandler(Node::Ptr node, Node::Ptr target, Node::Ptr parent)
{
	findSceneManager();

	if (_skin->duration() < 1e-6f)
		return; // incorrect animation

	if (node->hasComponent<Surface>())
	{
		auto geometry = node->component<Surface>()->geometry();

		if (geometry->hasVertexAttribute(ATTRNAME_POSITION) 
			&& geometry->vertexBuffer(ATTRNAME_POSITION)->numVertices() == _skin->numVertices())
		{
			_targetGeometry[node]			= geometry;
			_targetStartTime[node]			= (clock_t)(clock() / (float)CLOCKS_PER_SEC);

			_targetInputPositions[node]		= geometry->vertexBuffer(ATTRNAME_POSITION)->data();			

			if (geometry->hasVertexAttribute(ATTRNAME_NORMAL)
				&& geometry->vertexBuffer(ATTRNAME_NORMAL)->numVertices() == _skin->numVertices())
				_targetInputNormals[node]	= geometry->vertexBuffer(ATTRNAME_NORMAL)->data();

			if (_method != SkinningMethod::SOFTWARE)
			{
				geometry->addVertexBuffer(_boneVertexBuffer);
			
				UniformArrayPtr	uniformArray(new UniformArray(0, nullptr));
				geometry->data()->set<UniformArrayPtr>	(PNAME_BONE_MATRICES,	uniformArray);
				geometry->data()->set<int>				(PNAME_NUM_BONES,		0);
			}
		}
	}
}

void
Skinning::removedHandler(Node::Ptr, Node::Ptr target, Node::Ptr)
{
	findSceneManager();

	if (_targetGeometry.count(target) > 0)
	{
		auto geometry	= _targetGeometry[target];

		if (_method != SkinningMethod::SOFTWARE)
		{
			geometry->removeVertexBuffer(_boneVertexBuffer);
			geometry->data()->unset(PNAME_BONE_MATRICES);
			geometry->data()->unset(PNAME_NUM_BONES);
		}

		_targetGeometry.erase(target);
	}
	if (_targetStartTime.count(target) > 0)
		_targetStartTime.erase(target);
	if (_targetInputPositions.count(target) > 0)
		_targetInputPositions.erase(target);
	if (_targetInputNormals.count(target) > 0)
		_targetInputNormals.erase(target);
}

void
Skinning::findSceneManager()
{
	NodeSet::Ptr roots = NodeSet::create(targets())
		->roots()
		->where([](NodePtr node)
		{
			return node->hasComponent<SceneManager>();
		});

	if (roots->nodes().size() > 1)
		throw std::logic_error("Renderer cannot be in two separate scenes.");
	else if (roots->nodes().size() == 1)
		setSceneManager(roots->nodes()[0]->component<SceneManager>());		
	else
		setSceneManager(nullptr);
}

void
Skinning::setSceneManager(SceneManager::Ptr sceneManager)
{
	if (sceneManager)
	{
		_frameBeginSlot = sceneManager->frameBegin()->connect(std::bind(
			&Skinning::frameBeginHandler, 
			shared_from_this(), 
			std::placeholders::_1
		));
	}
	else if (_frameBeginSlot)
	{
		for (auto& target : targets())
		{
			//_started[target] = false;
			//stop(target);
		}

		_frameBeginSlot = nullptr;
	}
}

VertexBuffer::Ptr
Skinning::createVertexBufferForBones() const
{
	static const unsigned int vertexSize = 16;  // [bId0 bId1 bId2 bId3] [bId4 bId5 bId6 bId7] [bWgt0 bWgt1 bWgt2 bWgt3] [bWgt4 bWgt5 bWgt6 bWgt7]

	assert(_skin->maxNumVertexBones() <= MAX_NUM_BONES_PER_VERTEX);

	const unsigned int	numVertices	= _skin->numVertices();
	std::vector<float>	vertexData	(numVertices * vertexSize, 0.0f);

	unsigned int index	= 0;
	for (unsigned int vId = 0; vId < numVertices; ++vId)
	{
		const unsigned int numVertexBones = _skin->numVertexBones(vId);
	
		unsigned int j = 0;
		while(j < numVertexBones && j < (vertexSize >> 2))
		{
			vertexData[index + j] = (float)_skin->vertexBoneId(vId, j);
			++j;
		}
		index += (vertexSize >> 1);

		j = 0;
		while(j < numVertexBones && j < (vertexSize >> 2))
		{
			vertexData[index + j] = (float)_skin->vertexBoneWeight(vId, j);
			++j;
		}
		index += (vertexSize >> 1);
	}

#ifdef DEBUG_SKINNING
	assert(index == vertexData.size());
#endif // DEBUG_SKINNING

	auto vertexBuffer	= VertexBuffer::create(_context, vertexData);

	vertexBuffer->addAttribute(ATTRNAME_BONE_IDS_A,		4, 0);
	vertexBuffer->addAttribute(ATTRNAME_BONE_IDS_B,		4, 4);
	vertexBuffer->addAttribute(ATTRNAME_BONE_WEIGHTS_A,	4, 8);
	vertexBuffer->addAttribute(ATTRNAME_BONE_WEIGHTS_B,	4, 12);

	return vertexBuffer;
}


void
Skinning::frameBeginHandler(SceneManager::Ptr)
{
	const float time = clock() / (float)CLOCKS_PER_SEC;

	for (auto& target : targets())
		updateFrame(target, _skin->getFrameId(time - _targetStartTime[target]));
}

void
Skinning::updateFrame(Node::Ptr		target, 
					  unsigned int	frameId)
{
	if (_targetGeometry.count(target) == 0 || frameId >= _skin->numFrames())
		return;

	auto&						geometry		= _targetGeometry[target];
	const std::vector<float>&	boneMatrices	= _skin->matrices(frameId);

	if (_method == SkinningMethod::HARDWARE)
	{
		geometry->data()->set<int>(PNAME_NUM_BONES,	_skin->numBones());
	
		const auto& uniformArray	= geometry->data()->get<UniformArrayPtr>	(PNAME_BONE_MATRICES);
		uniformArray->first			= _skin->numBones();
		uniformArray->second		= &(boneMatrices[0]); 
	}
	else
		performSoftwareSkinning(target, boneMatrices);
}

void
Skinning::performSoftwareSkinning(Node::Ptr					target, 
								 const std::vector<float>&	boneMatrices)
{
#ifdef DEBUG_SKINNING
	assert(target && _targetGeometry.count(target) > 0 && _targetInputPositions.count(target) > 0);
#endif //DEBUG_SKINNING
	
	auto geometry	= _targetGeometry[target];

	// transform positions
	auto						xyzBuffer	= geometry->vertexBuffer(ATTRNAME_POSITION);
	VertexBuffer::AttributePtr	xyzAttr		= nullptr;
	for (auto& attr : xyzBuffer->attributes())
		if (std::get<0>(*attr) == ATTRNAME_POSITION)
			xyzAttr = attr;

	performSoftwareSkinning(xyzAttr, xyzBuffer, _targetInputPositions[target], boneMatrices, false);

	// transform normals
	if (geometry->hasVertexAttribute(ATTRNAME_NORMAL) && _targetInputNormals.count(target) > 0)
	{
		auto						normalBuffer	= geometry->vertexBuffer(ATTRNAME_NORMAL);
		VertexBuffer::AttributePtr	normalAttr		= nullptr;
		for (auto& attr : normalBuffer->attributes())
			if (std::get<0>(*attr) == ATTRNAME_NORMAL)
				normalAttr = attr;

		performSoftwareSkinning(normalAttr, normalBuffer, _targetInputNormals[target], boneMatrices, true);
	}
}

void
Skinning::performSoftwareSkinning(VertexBuffer::AttributePtr		attr,
								 VertexBuffer::Ptr					vertexBuffer, 
								 const std::vector<float>&			inputData,
								 const std::vector<float>&			boneMatrices,
								 bool								doDeltaTransform)
{
#ifdef DEBUG_SKINNING
	assert(vertexBuffer && vertexBuffer->data().size() == inputData.size());
	assert(attr && std::get<1>(*attr) == 3);
	assert(boneMatrices.size() == (_skin->numBones() << 4));
#endif // DEBUG_SKINNING

	const unsigned int	numBones		= _skin->numBones();
	const unsigned int	vertexSize		= vertexBuffer->vertexSize();
	std::vector<float>&	outputData		= vertexBuffer->data();
	const unsigned int	numVertices		= outputData.size() / vertexSize;
	
#ifdef DEBUG_SKINNING
	assert(numVertices == _skin->numVertices());
#endif // DEBUG_SKINNING

	unsigned int index = std::get<2>(*attr);
	for (unsigned int vId = 0; vId < numVertices; ++vId)
	{
		const float x1 = inputData[index];
		const float y1 = inputData[index+1];
		const float z1 = inputData[index+2];

		float x2 = 0.0f;
		float y2 = 0.0f;
		float z2 = 0.0f;

		const unsigned int numVertexBones = _skin->numVertexBones(vId);
		for (unsigned int j = 0; j < numVertexBones; ++j)
		{
			unsigned int	boneId		= 0;
			float			boneWeight	= 0.0f;

			_skin->vertexBoneData(vId, j, boneId, boneWeight);

			const float*	boneMatrix	= &(boneMatrices[boneId << 4]);

			if (!doDeltaTransform)
			{
				x2 += boneWeight * (boneMatrix[0] * x1 + boneMatrix[1] * y1 + boneMatrix[2]  * z1 + boneMatrix[3]);
				y2 += boneWeight * (boneMatrix[4] * x1 + boneMatrix[5] * y1 + boneMatrix[6]  * z1 + boneMatrix[7]);
				z2 += boneWeight * (boneMatrix[8] * x1 + boneMatrix[9] * y1 + boneMatrix[10] * z1 + boneMatrix[11]);
			}
			else
			{
				x2 += boneWeight * (boneMatrix[0] * x1 + boneMatrix[1] * y1 + boneMatrix[2]  * z1);
				y2 += boneWeight * (boneMatrix[4] * x1 + boneMatrix[5] * y1 + boneMatrix[6]  * z1);
				z2 += boneWeight * (boneMatrix[8] * x1 + boneMatrix[9] * y1 + boneMatrix[10] * z1);
			}
		}

		outputData[index]	= x2;
		outputData[index+1]	= y2;
		outputData[index+2]	= z2;

		index += vertexSize;
	}

	vertexBuffer->upload();
}