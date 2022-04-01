/*
 *
 * Confidential Information of Telekinesys Research Limited (t/a Havok). Not for disclosure or distribution without Havok's
 * prior written consent. This software contains code, techniques and know-how which is confidential and proprietary to Havok.
 * Product and Trade Secret source code contains trade secrets of Havok. Havok Software (C) Copyright 1999-2014 Telekinesys Research Limited t/a Havok. All Rights Reserved. Use of this software is subject to the terms of an end user license agreement.
 *
 */

#include "FbxToHkxConverter.h"

#include <Common/Base/hkBase.h>
#include <Common/Base/Math/hkMath.h>
#include <Common/SceneData/Mesh/hkxMesh.h>
#include <Common/SceneData/Mesh/hkxMeshSection.h>
#include <Common/Serialize/Util/hkSerializeUtil.h>
#include <Common/SceneData/Scene/hkxScene.h>
#include <Common/Serialize/Util/hkRootLevelContainer.h>
#include <Common/Base/System/Io/IStream/hkIStream.h>
#include <Common/Serialize/Resource/hkResource.h>
#include <Common/SceneData/Environment/hkxEnvironment.h>
#include <Common/Serialize/ResourceDatabase/hkResourceHandle.h>
#include <Common/Base/Ext/hkBaseExt.h>
#include <Common/Base/Fwd/hkwindows.h>

#include <Common/Base/Algorithm/Sort/hkSort.h>

// Get the matrix of the given pose
FbxAMatrix GetPoseMatrix(FbxPose* pPose, int pNodeIndex);

// Get the geometry offset to a node. It is never inherited by the children.
FbxAMatrix GetGeometry(FbxNode* pNode);

static void GetCustomVisionData(FbxNode* fbxNode, hkStringPtr& userPropertiesStr);
static void PrintLine();

//-------

FbxToHkxConverter::Options::Options(FbxManager *fbxSdkManager) :
	m_fbxSdkManager(fbxSdkManager),
	m_exportMeshes(true), m_exportMaterials(true), m_exportAttributes(true),
	m_exportAnnotations(true), m_exportLights(true), m_exportCameras(true),
	m_exportVertexAnimations(true),
	m_visibleOnly(false), m_selectedOnly(false)
{
	HK_ASSERT(0x0, m_fbxSdkManager);
}

FbxToHkxConverter::FbxToHkxConverter(const Options& options) : 
	m_options(options), m_curFbxScene(NULL), m_pose(NULL)
{
}

FbxToHkxConverter::~FbxToHkxConverter()
{
	clear();
}

void FbxToHkxConverter::clear()
{
	for (int sceneIndex = 0; sceneIndex < m_scenes.getSize(); sceneIndex++)
	{
		hkxScene *scene = m_scenes[sceneIndex];
		scene->removeReference();
	}
	m_scenes.clear();

	for(hkPointerMap<FbxTexture*, hkRefVariant*>::Iterator it = m_convertedTextures.getIterator(); m_convertedTextures.isValid(it); it = m_convertedTextures.getNext(it))
	{
		hkRefVariant* var = m_convertedTextures.getValue(it);
		if (var)
		{
			delete var;
		}
	}
	m_convertedTextures.clear();
}

void FbxToHkxConverter::saveScenes(const char* path, const char* name)
{
	printf("Output path: %s\n", path);

	for (int sceneIndex = 0; sceneIndex < m_scenes.getSize(); sceneIndex++)
	{
		hkxScene *scene = m_scenes[sceneIndex];
		hkRootLevelContainer* currentRootContainer = new hkRootLevelContainer();
		currentRootContainer->m_namedVariants.setSize(1);

		hkRootLevelContainer::NamedVariant& sceneVariant = currentRootContainer->m_namedVariants[0];
		sceneVariant.set("Scene Data", scene, &hkxSceneClass);

		hkStringBuf filename = name;

		if (sceneIndex > 0)
		{
			filename.append("_");

			hkStringBuf name = scene->m_rootNode->m_name;

			char invalid_characters[] = { ' ', '.', '/', '?', '<', '>', '\\', ':', '*', '|' };
			for (int character_index = 0; character_index < sizeof(invalid_characters); character_index++ )
			{
				name.replace(invalid_characters[character_index], '_');
			}

			filename.append( name );
		}

		PrintLine();

		hkStringBuf tagfile = filename;
		tagfile.append(".hkt");

		hkStringBuf tagpath = path;
		tagpath.pathNormalize();

		if (tagpath.getLength() > 0 && !tagpath.endsWith("/"))
		{
			tagpath.append("/");
		}

		tagpath.append(tagfile);

		if ( hkSerializeUtil::save(
				currentRootContainer,
				hkRootLevelContainerClass,
				hkOstream(tagpath).getStreamWriter(),
				hkSerializeUtil::SAVE_TEXT_FORMAT) == HK_SUCCESS )
		{
			printf("Saved tag file: %s\n", tagfile.cString());
		}
		else
		{
			printf("Cannot save file: %s\n", tagfile.cString());
		}

		printf("Scene length: %0.2f\n", scene->m_sceneLength);
		printf("Root node name: %s\n", scene->m_rootNode->m_name.cString());

		delete currentRootContainer;
	}
}

// This method is templated on the implementation of hctMayaSceneExporter/hctMaxSceneExporter::createScene()
bool FbxToHkxConverter::createScenes(FbxScene* fbxScene, bool noTakes)
{
	clear();

	m_curFbxScene = fbxScene;
	m_rootNode = m_curFbxScene->GetRootNode();

	m_modeller = "FBX";
	hkStringBuf application = fbxScene->GetSceneInfo()->Original_ApplicationName.Get();
	if (application.getLength() > 0)
	{
		m_modeller += " [";
		m_modeller += application;
		m_modeller += "]";
	}
	printf("Modeller: %s\n", m_modeller.cString());

	if (m_options.m_selectedOnly)
	{
		printf("Exporting Selected Only\n");
	}

	if (m_options.m_visibleOnly)
	{
		printf("Exporting Visible Only\n");
	}

	hkArray<FbxNode*> boneNodes;
	findChildren(m_rootNode, boneNodes, FbxNodeAttribute::eSkeleton);
	m_numBones = boneNodes.getSize();
	printf("Bones: %d\n", m_numBones);

	const int poseCount = m_curFbxScene->GetPoseCount();
	if (poseCount > 0)
	{
		m_pose = m_curFbxScene->GetPose(0);
		printf("Pose Elements: %d\n", m_pose->GetCount());		
	}

	m_numAnimStacks = m_curFbxScene->GetSrcObjectCount<FbxAnimStack>();
	if (m_numAnimStacks > 0)
	{
		const FbxAnimStack* lAnimStack = m_curFbxScene->GetSrcObject<FbxAnimStack>(0);
		const FbxTimeSpan animTimeSpan = lAnimStack->GetLocalTimeSpan();
		FbxTime timePerFrame; timePerFrame.SetTime(0, 0, 0, 1, 0, m_curFbxScene->GetGlobalSettings().GetTimeMode());
		m_startTime = animTimeSpan.GetStart();
	}
	
	if (noTakes)
	{
		if (m_numAnimStacks > 0)
		{
			printf("'-noTakes' option set, only exporting first animation.\n");
			createSceneStack(0);
		}
		else
		{
			printf("'-noTakes' option set and no animation present, only exporting static geometry.\n");
			createSceneStack(-1);
		}
	}
	else
	{
		printf("Animation stacks: %d\n", m_numAnimStacks);
		createSceneStack(-1);

		for (int animStackIndex = 0;
			animStackIndex < m_numAnimStacks && m_numBones > 0;
			animStackIndex++)
		{
			createSceneStack(animStackIndex);
		}
	}

	return true;
}

// This method is templated on the implementation of hctMayaSceneExporter/hctMaxSceneExporter::createScene()
bool FbxToHkxConverter::createSceneStack(int animStackIndex)
{
	hkxScene *scene = new hkxScene;
	hkUint32 numFrames = 0;

	scene->m_modeller.set(m_modeller.cString());
	scene->m_asset = m_curFbxScene->GetSceneInfo()->Original_FileName.Get();

	if (m_rootNode) 
	{
		// create root node
		hkxNode* rootNode = new hkxNode;
		bool rigPass = (animStackIndex == -1);
		int currentAnimStackIndex = -1;

		FbxAnimStack* lAnimStack = NULL;
		
		if (rigPass && m_numAnimStacks > 0)
		{
			currentAnimStackIndex = 0;
		}
		else if (animStackIndex >= 0)
		{
			currentAnimStackIndex = animStackIndex;
		}
		
		if (currentAnimStackIndex != -1)
		{
			lAnimStack = m_curFbxScene->GetSrcObject<FbxAnimStack>(currentAnimStackIndex);
			m_curFbxScene->SetCurrentAnimationStack(lAnimStack);
		}

		if (rigPass)
		{
			rootNode->m_name = "ROOT_NODE";
			scene->m_sceneLength = 0.f;
			numFrames = 1;
			printf("Converting nodes for root...\n");
		}
		else
		{
			rootNode->m_name = lAnimStack->GetName();

			const FbxTimeSpan animTimeSpan = lAnimStack->GetLocalTimeSpan();
			scene->m_sceneLength = static_cast<hkReal>( animTimeSpan.GetDuration().GetSecondDouble() );
			numFrames = static_cast<hkUint32>( animTimeSpan.GetDuration().GetFrameCount(m_curFbxScene->GetGlobalSettings().GetTimeMode()) );
			
			printf("Converting nodes for [%s]...\n", rootNode->m_name.cString());
		}

		scene->m_rootNode = rootNode;
		rootNode->removeReference();

		// Setup (identity) keyframes(s) for the 'static' root node
		rootNode->m_keyFrames.setSize( numFrames > 1 ? 2 : 1, hkMatrix4::getIdentity() );

		addNodesRecursive(scene, m_rootNode, scene->m_rootNode, currentAnimStackIndex);
	}

	m_scenes.pushBack(scene);

	return true;
}

// This method is templated on the implementation of hctMayaSceneExporter::createHkxNodes()
void FbxToHkxConverter::addNodesRecursive(hkxScene *scene, FbxNode* fbxNode, hkxNode* node, int animStackIndex)
{
	for (int childIndex = 0; childIndex < fbxNode->GetChildCount(); childIndex++)
	{
		FbxNode* fbxChildNode = fbxNode->GetChild(childIndex);
		FbxNodeAttribute* fbxNodeAtttrib = fbxChildNode->GetNodeAttribute();
		bool selected = fbxChildNode->GetSelected();

		// Ignore nodes(and their descendants) if they're invisible and we ignore invisible objects
		if ( !(!m_options.m_visibleOnly || fbxNode->GetVisibility()) )
			continue;

		// Ignore nodes(and their descendants) if they're not selected and we ignore deselected objects
		if ( !(!m_options.m_selectedOnly || selected) )
			continue;

		hkxNode* newChildNode = new hkxNode();
		{
			newChildNode->m_name = fbxChildNode->GetName();
			node->m_children.pushBack(newChildNode);
		}

		newChildNode->m_selected = selected;

		// Extract the following types of data from this node (taken from hkxScene.h):
		if (fbxNodeAtttrib != NULL)
		{
			switch (fbxNodeAtttrib->GetAttributeType())
			{
			case FbxNodeAttribute::eMesh:
				{
					// Generate hkxMesh and all its dependent data (ie: hkxSkinBinding, hkxMeshSection, hkxMaterial)
					if (m_options.m_exportMeshes)
					{
						addMesh(scene, fbxChildNode, newChildNode);
					}
					break;
				}
			case FbxNodeAttribute::eCamera:
				{
					// Generate hkxCamera
					if (m_options.m_exportCameras)
					{
						addCamera(scene, fbxChildNode, newChildNode);
					}
					break;
				}
			case FbxNodeAttribute::eLight:
				{
					// Generate hkxLight
					if (m_options.m_exportLights)
					{
						addLight(scene, fbxChildNode, newChildNode);
					}
					break;
				}
			default:
				break;
			}		
		}

		// Extract this node's animation data and bind transform
		extractKeyFramesAndAnnotations(scene, fbxChildNode, newChildNode, animStackIndex);

		if (m_options.m_exportAttributes)
		{
			addSampledNodeAttributeGroups(scene, animStackIndex, fbxChildNode, newChildNode);
		}

		GetCustomVisionData(fbxChildNode, newChildNode->m_userProperties);

		addNodesRecursive(scene, fbxChildNode, newChildNode, animStackIndex);
		newChildNode->removeReference();
	}
}

void FbxToHkxConverter::extractKeyFramesAndAnnotations(hkxScene *scene, FbxNode* fbxChildNode, hkxNode* newChildNode, int animStackIndex)
{
	FbxAMatrix bindPoseMatrix;
	FbxAnimStack* lAnimStack = NULL;
	int numAnimLayers = 0;
	FbxTimeSpan animTimeSpan;
	
	if (animStackIndex >= 0)
	{
		lAnimStack = m_curFbxScene->GetSrcObject<FbxAnimStack>(animStackIndex);
		numAnimLayers = lAnimStack->GetMemberCount<FbxAnimLayer>();
		animTimeSpan = lAnimStack->GetLocalTimeSpan();
	}

	// Find the time offset (in the "time space" of the FBX file) of the first animation frame
	FbxTime timePerFrame; timePerFrame.SetTime(0, 0, 0, 1, 0, m_curFbxScene->GetGlobalSettings().GetTimeMode());
	const FbxTime startTime = animTimeSpan.GetStart();
	const FbxTime endTime = animTimeSpan.GetStop();

	const hkReal startTimeSeconds = static_cast<hkReal>(startTime.GetSecondDouble());
	const hkReal endTimeSeconds = static_cast<hkReal>(endTime.GetSecondDouble());

	int numFrames = 0;
	bool staticNode = true;

	if (scene->m_sceneLength == 0)
	{
		bindPoseMatrix = fbxChildNode->EvaluateLocalTransform(startTime);
	}
	else
	{
		hkArray<hkStringOld> annotationStrings;
		hkArray<hkReal> annotationTimes;

		HK_ASSERT(0x0, newChildNode->m_keyFrames.getSize() == 0);

		// Sample each animation frame
		for (FbxTime time = startTime, priorSampleTime = endTime;
			 time < endTime;
			 priorSampleTime = time, time += timePerFrame, ++numFrames)
		{
			FbxAMatrix frameMatrix = fbxChildNode->EvaluateLocalTransform(time);
			staticNode = staticNode && (frameMatrix == bindPoseMatrix);

			hkMatrix4 mat;

			// Extract this frame's transform
			convertFbxXMatrixToMatrix4(frameMatrix, mat);
			newChildNode->m_keyFrames.pushBack(mat);

			// Extract all annotation strings for this frame using the deprecated
			// pipeline (new annotations are extracted when sampling attributes)
			if (m_options.m_exportAnnotations && numAnimLayers > 0)
			{
				FbxProperty prop = fbxChildNode->GetFirstProperty();
				while(prop.IsValid())
				{
					FbxString propName  = prop.GetName();
					FbxDataType lDataType = prop.GetPropertyDataType();
					if (lDataType.GetType() == eFbxEnum && propName.Size() >= 2 && hkString::strNcasecmp(propName.Buffer(), "HK", 2) == 0)
					{
						FbxAnimLayer* lAnimLayer = lAnimStack->GetMember<FbxAnimLayer>(0);
						FbxAnimCurve* lAnimCurve = prop.GetCurve(lAnimLayer);

						int currentKeyIndex;
						const int keyIndex = (int)lAnimCurve->KeyFind(time, &currentKeyIndex);
						const int priorKeyIndex = (int) lAnimCurve->KeyFind(priorSampleTime);

						// Only store annotations on frames where they're explicitly keyframed, or if this is the first keyframe 
						if (priorKeyIndex != keyIndex)
						{
							const int currentEnumValueIndex = keyIndex < 0 ? (int) lAnimCurve->Evaluate(priorSampleTime) : (int) lAnimCurve->Evaluate(time);
							HK_ASSERT(0x0, currentEnumValueIndex < prop.GetEnumCount());
							const char* enumValue = prop.GetEnumValue(currentEnumValueIndex);
							hkxNode::AnnotationData& annotation = newChildNode->m_annotations.expandOne();
							annotation.m_time = (hkReal) (time - startTime).GetSecondDouble();

							hkStringBuf description(propName.Buffer(), enumValue);
							annotation.m_description = description;
						}
					}
					prop = fbxChildNode->GetNextProperty(prop);
				}
			}
		}
	}

	// Replace animation key data for static nodes with just 1 or 2 frames of bind pose data
	if (staticNode)
	{
		// Static nodes in animated scene data are exported with two keys
		const bool exportTwoFramesForStaticNodes = (numFrames > 1);

		// replace transform
		newChildNode->m_keyFrames.setSize(exportTwoFramesForStaticNodes ? 2: 1);
		newChildNode->m_keyFrames.optimizeCapacity(0, true);

		// convert the bind pose transform to Havok format
		convertFbxXMatrixToMatrix4(bindPoseMatrix, newChildNode->m_keyFrames[0]);

		if (exportTwoFramesForStaticNodes)
		{
			newChildNode->m_keyFrames[1] = newChildNode->m_keyFrames[0];
		}
	}
}

void FbxToHkxConverter::findChildren(FbxNode* root, hkArray<FbxNode*>& children, FbxNodeAttribute::EType type)
{
	for (int childIndex = 0; childIndex < root->GetChildCount(); childIndex++)
	{
		FbxNode *node = root->GetChild(childIndex);
		if (node->GetNodeAttribute() != NULL &&
			node->GetNodeAttribute()->GetAttributeType() == type)
		{
			children.pushBack(node);
		}

		findChildren(node, children, type);
	}
}

FbxAMatrix FbxToHkxConverter::getGlobalPosition(FbxNode* pNode, const FbxTime& pTime, FbxPose* pPose, FbxAMatrix* pParentGlobalPosition)
{
	FbxAMatrix lGlobalPosition;
	bool        lPositionFound = false;

	if (pPose)
	{
		int lNodeIndex = pPose->Find(pNode);

		if (lNodeIndex > -1)
		{
			// The bind pose is always a global matrix.
			// If we have a rest pose, we need to check if it is
			// stored in global or local space.
			if (pPose->IsBindPose() || !pPose->IsLocalMatrix(lNodeIndex))
			{
				lGlobalPosition = GetPoseMatrix(pPose, lNodeIndex);
			}
			else
			{
				// We have a local matrix, we need to convert it to
				// a global space matrix.
				FbxAMatrix lParentGlobalPosition;

				if (pParentGlobalPosition)
				{
					lParentGlobalPosition = *pParentGlobalPosition;
				}
				else
				{
					if (pNode->GetParent())
					{
						lParentGlobalPosition = getGlobalPosition(pNode->GetParent(), pTime, pPose);
					}
				}

				FbxAMatrix lLocalPosition = GetPoseMatrix(pPose, lNodeIndex);
				lGlobalPosition = lParentGlobalPosition * lLocalPosition;
			}

			lPositionFound = true;
		}
	}

	if (!lPositionFound)
	{
		// There is no pose entry for that node, get the current global position instead.

		// Ideally this would use parent global position and local position to compute the global position.
		// Unfortunately the equation 
		//    lGlobalPosition = pParentGlobalPosition * lLocalPosition
		// does not hold when inheritance type is other than "Parent" (RSrs).
		// To compute the parent rotation and scaling is tricky in the RrSs and Rrs cases.
		lGlobalPosition = pNode->EvaluateGlobalTransform(pTime);
	}

	return lGlobalPosition;
}

//-------

FbxAMatrix GetPoseMatrix(FbxPose* pPose, int pNodeIndex)
{
	FbxAMatrix lPoseMatrix;
	FbxMatrix lMatrix = pPose->GetMatrix(pNodeIndex);

	memcpy((double*)lPoseMatrix, (double*)lMatrix, sizeof(lMatrix.mData));

	return lPoseMatrix;
}

static void GetCustomVisionData(FbxNode* fbxNode, hkStringPtr& userPropertiesStr)
{
	hkStringBuf propertyBuf = "";
	// When exported from max the user data will just be one big string.
	// As parsing can be done in the filter manager we just copy all string properties into one string.
	for(FbxProperty prop = fbxNode->GetFirstProperty(); prop.IsValid(); prop = fbxNode->GetNextProperty(prop))
	{
		FbxString propertyName = prop.GetName();
		EFbxType type = prop.GetPropertyDataType().GetType();
		const char* name = propertyName.Buffer();
		
		switch (type)
		{
		case eFbxChar:
		case eFbxShort:
		case eFbxUInt:
		case eFbxLongLong:
			{
				FbxLongLong propertyData = prop.Get<FbxLongLong>();
				propertyBuf.appendPrintf("%s = %lld\n", name, propertyData);
			}
			break;
		case eFbxUChar:
		case eFbxUShort:
		case eFbxInt:
		case eFbxULongLong:
			{
				FbxULongLong propertyData = prop.Get<FbxULongLong>();
				propertyBuf.appendPrintf("%s = %llu\n", name, propertyData);
			}
			break;
		case eFbxBool:
			{
				FbxBool propertyData = prop.Get<FbxBool>();
				propertyBuf.appendPrintf("%s = %s\n", name, propertyData ? "1" : "0");
			}
			break;
		case eFbxHalfFloat:
		case eFbxFloat:
		case eFbxDouble:
			{
				FbxDouble propertyData = prop.Get<FbxDouble>();
				propertyBuf.appendPrintf("%s = %f\n", name, propertyData);
			}
			break;
		//case fbxsdk_2014_2_1::eFbxEnum:
		//  break;
		case eFbxString:
			{
				// The Maya and Max Vision plugins write all properties into this single variable:
				if (hkString::strCmp("UDP3DSMAX", name) == 0)
				{
					FbxString propertyData = prop.Get<FbxString>();
					const char* text = propertyData.Buffer();
					propertyBuf.appendJoin(text, "\n");
				}
				else
				{
					FbxString propertyData = prop.Get<FbxString>();
					const char* text = propertyData.Buffer();
					propertyBuf.appendJoin(name, " = ", text, "\n");
				}
			}
			break;
		default:
			break;
		}
	}

	userPropertiesStr = propertyBuf;
}

FbxAMatrix GetGeometry(FbxNode* pNode)
{
	const FbxVector4 lT = pNode->GetGeometricTranslation(FbxNode::eSourcePivot);
	const FbxVector4 lR = pNode->GetGeometricRotation(FbxNode::eSourcePivot);
	const FbxVector4 lS = pNode->GetGeometricScaling(FbxNode::eSourcePivot);

	return FbxAMatrix(lT, lR, lS);
}

static void PrintLine()
{
	printf("-------------------------------------------------------------------------------\n");
}

/*
 * Havok SDK - NO SOURCE PC DOWNLOAD, BUILD(#20140907)
 * 
 * Confidential Information of Havok.  (C) Copyright 1999-2014
 * Telekinesys Research Limited t/a Havok. All Rights Reserved. The Havok
 * Logo, and the Havok buzzsaw logo are trademarks of Havok.  Title, ownership
 * rights, and intellectual property rights in the Havok software remain in
 * Havok and/or its suppliers.
 * 
 * Use of this software for evaluation purposes is subject to and indicates
 * acceptance of the End User licence Agreement for this product. A copy of
 * the license is included with this software and is also available at www.havok.com/tryhavok.
 * 
 */
