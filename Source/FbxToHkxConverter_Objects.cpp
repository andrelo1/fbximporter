/*
 *
 * Confidential Information of Telekinesys Research Limited (t/a Havok). Not for disclosure or distribution without Havok's
 * prior written consent. This software contains code, techniques and know-how which is confidential and proprietary to Havok.
 * Product and Trade Secret source code contains trade secrets of Havok. Havok Software (C) Copyright 1999-2014 Telekinesys Research Limited t/a Havok. All Rights Reserved. Use of this software is subject to the terms of an end user license agreement.
 *
 */

#include "FbxToHkxConverter.h"
#include <Common/SceneData/Scene/hkxSceneUtils.h>
#include <Common/SceneData/Mesh/hkxMeshSectionUtil.h>

template <class T>
void convertPropertyToVector4(const FbxPropertyT<T> &property, hkVector4 &vec, float z = 0.0f)
{
	vec.set( static_cast<float>(property.Get()[0]),
			 static_cast<float>(property.Get()[1]),
			 static_cast<float>(property.Get()[2]),
			 z );
}

template <typename T>
unsigned elementsToARGB(const T r, const T g, const T b, const T a) 
{
	return (static_cast<unsigned char>(static_cast<float>(a) * 255.0f) << 24) |
			 (static_cast<unsigned char>(static_cast<float>(r) * 255.0f) << 16) |
			 (static_cast<unsigned char>(static_cast<float>(g) * 255.0f) << 8) |
			 (static_cast<unsigned char>(static_cast<float>(b) * 255.0f));
}

FbxAMatrix FbxToHkxConverter::convertMatrix(const FbxMatrix& mat)
{
	FbxVector4 trans, shear, scale;
	FbxQuaternion rot;
	double sign;
	mat.GetElements(trans, rot, shear, scale, sign);
	FbxAMatrix ret;
	ret.SetT(trans);
	ret.SetQ(rot);
	ret.SetS(scale);
	return ret;
}

void FbxToHkxConverter::addCamera(hkxScene *scene, FbxNode* cameraNode, hkxNode* node)
{
	hkxCamera* newCamera = new hkxCamera();

	FbxCamera* cameraAttrib =(FbxCamera*)cameraNode->GetNodeAttribute();
	HK_ASSERT(0x0, cameraAttrib->GetAttributeType()== FbxNodeAttribute::eCamera);

	FbxDouble3 pos = cameraAttrib->Position.Get();
	newCamera->m_from.set((hkReal)pos[0],(hkReal)pos[1],(hkReal)pos[2]);
	FbxDouble3 up = cameraAttrib->UpVector.Get();
	newCamera->m_up.set((hkReal)up[0],(hkReal)up[1],(hkReal)up[2]);
	FbxDouble3 focus = cameraAttrib->InterestPosition.Get();
	newCamera->m_focus.set((hkReal)focus[0],(hkReal)focus[1],(hkReal)focus[2]);

	const hkReal degreesToRadians  = HK_REAL_PI / 180.0f;
	newCamera->m_fov =(hkReal)cameraAttrib->FieldOfViewY.Get()* degreesToRadians;
	newCamera->m_near =(hkReal)cameraAttrib->NearPlane.Get();
	newCamera->m_far =(hkReal)cameraAttrib->FarPlane.Get();

	newCamera->m_leftHanded = false;

	node->m_object = newCamera;
	scene->m_cameras.pushBack(newCamera);
	newCamera->removeReference();
}

void FbxToHkxConverter::addLight(hkxScene *scene, FbxNode* lightNode, hkxNode* node)
{
	hkxLight* newLight = new hkxLight();

	FbxLight* lightAttrib =(FbxLight*)lightNode->GetNodeAttribute();
	HK_ASSERT(0x0, lightAttrib->GetAttributeType()== FbxNodeAttribute::eLight);

	const FbxAMatrix& lightTransform = lightNode->EvaluateLocalTransform();
	const FbxVector4& lightPos = lightTransform.GetT();
	newLight->m_position.set((hkReal)lightPos[0],(hkReal)lightPos[1],(hkReal)lightPos[2]);

	// FBX lights point along their node's negative Y axis
	const FbxVector4& negLightDir = lightTransform.GetRow(1);
	newLight->m_direction.set((hkReal)-negLightDir[0],(hkReal)-negLightDir[1],(hkReal)-negLightDir[2]);

	const FbxDouble3 color = lightAttrib->Color.Get();
	newLight->m_color = elementsToARGB(color[0], color[1], color[2], 1.0); 

	switch (lightAttrib->LightType.Get())
	{
	case FbxLight::ePoint:
		{
			HK_WARN_ALWAYS(0x0, "Point lights are not supported. Please use a directional light, or ambient lighting instead.");
			newLight->removeReference();
			newLight = HK_NULL;
			break;
		}
	case FbxLight::eDirectional:
		{
			newLight->m_type = hkxLight::DIRECTIONAL_LIGHT;
			break;
		}
	case FbxLight::eSpot:
		{
			newLight->m_angle =(hkReal)lightAttrib->InnerAngle.Get();
			newLight->m_type = hkxLight::SPOT_LIGHT;
			break;
		}
	default:
		HK_WARN(0x0, "Unsupported light type encountered. Expected Point, Directional, or Spot light.");
		break;
	}

	if (newLight)
	{
		node->m_object = newLight;
		scene->m_lights.pushBack(newLight);
		newLight->removeReference();
	}
}

static hkxMaterial* createDefaultMaterial(const char* name)
{
	hkxMaterial* mat = new hkxMaterial();
	mat->m_name = name;
	mat->m_diffuseColor.set(1.0f, 1.0f, 1.0f, 1.0f);
	mat->m_ambientColor.setAll(0);
	mat->m_specularColor = mat->m_diffuseColor;
	mat->m_specularColor(3)= 75.0f; // Spec power
	mat->m_emissiveColor.setAll(0);
	return mat;
}

void FbxToHkxConverter::addMesh(hkxScene *scene, FbxNode* meshNode, hkxNode* node)
{
	FbxMesh* originalMesh = meshNode->GetMesh();
	FbxMesh* triMesh = NULL;

	if (!originalMesh->IsTriangleMesh())
	{
		 FbxGeometryConverter lGeometryConverter(m_options.m_fbxSdkManager);
		 triMesh = static_cast<FbxMesh*>( lGeometryConverter.Triangulate(meshNode->GetNodeAttribute(), false) );
	}
	else
	{
		triMesh = originalMesh;
	}

	hkxMesh* newMesh = HK_NULL;
	hkxSkinBinding* newSkin = HK_NULL;


	// Get materials
	hkArray<FbxSurfaceMaterial*> matIds;
	getMaterialsInMesh(triMesh, matIds);

	// Each matId maps to a mesh section.
	hkArray<hkxMeshSection*> exportedSections;
	exportedSections.reserve( matIds.getSize() );


	// Get skinning info
	const int lSkinCount = triMesh->GetDeformerCount(FbxDeformer::eSkin);
	FbxSkin *skin = (FbxSkin *)triMesh->GetDeformer(0, FbxDeformer::eSkin);

	hkArray<float> skinControlPointWeights;
	hkArray<int> skinIndicesToClusters;
	{
		if (lSkinCount>0)
		{
			const int skinDataCount = triMesh->GetControlPointsCount()*4;
			skinControlPointWeights.setSize(skinDataCount,0.0f);
			skinIndicesToClusters.setSize(skinDataCount,-1);
	
			const int lClusterCount = skin->GetClusterCount();
			for (int curClusterIndex=0; curClusterIndex < lClusterCount; ++curClusterIndex)
			{
				FbxCluster* lCluster = skin->GetCluster(curClusterIndex);
				const int lIndexCount = lCluster->GetControlPointIndicesCount();
				int* lIndices = lCluster->GetControlPointIndices();
				double* lWeights = lCluster->GetControlPointWeights();
	
				for (int k = 0; k < lIndexCount; k++)
				{
					const int controlPointIndexFour = lIndices[k] * 4;
					for(int i = controlPointIndexFour; i < controlPointIndexFour + 4; ++i)
					{
						if (skinIndicesToClusters[i] < 0)
						{
							skinIndicesToClusters[i] = curClusterIndex;
							skinControlPointWeights[i] =(float)lWeights[k];
							break;
						}
					}
				}
			}
		}
	
		// Zero unused indices
		for (int i = 0; i <skinIndicesToClusters.getSize(); ++i)
		{
			if (skinIndicesToClusters[i] < 0)
			{
				skinIndicesToClusters[i] = 0;
			}
		}
	}

	// FbxGeometryElementMaterial maps polygons to materials. We currently do not support
	// mapping a polygon to multiple materials so we only consider the first mapping.
	const FbxGeometryElementMaterial* elemMat = triMesh->GetElementMaterial(0);
	FbxLayerElement::EMappingMode mode;
	if (elemMat)
	{
		mode = elemMat->GetMappingMode();
	}
	else
	{
		// If there is no material mapping we create a dummy material and map everything to it.
		mode = FbxLayerElement::eAllSame;
		// If there is no material mapping there also shouldn't be any materials.
		// Nevertheless we check this just to be sure.
		if (matIds.isEmpty())
		{
			// Our dummy material needed for the mesh section has no matching counterpart on the fbx side.
			matIds.pushBack(NULL);
		}
	}
	
	

	// Create subsection for each material
	const int materialCount = matIds.getSize();
	for (int curMat = 0; curMat < materialCount; ++curMat)
	{
		hkArray<int> materialIndices;
		materialIndices.reserve(triMesh->GetPolygonCount());
		if (mode == FbxLayerElement::eAllSame)
		{
			// The material is used for all triangles. To be able to use the same code in this case
			// we just write all indices into the array.
			const int polygonCount = triMesh->GetPolygonCount();
			for (int i = 0; i < polygonCount; ++i)
			{
				materialIndices.pushBack(i);
			}
		}
		else if (mode == FbxLayerElement::eByPolygon)
		{
			FbxLayerElementArrayTemplate<int>& indexArray = elemMat->GetIndexArray();
			const int indexCount = indexArray.GetCount();
			for (int i = 0; i < indexCount; ++i)
			{
				if (indexArray[i] == curMat)
				{
					materialIndices.pushBack(i);
				}
			}
		}
		else
		{
			HK_WARN(0x0, "Unsupported material mapping mode. This material will be ignored.");
			continue;
		}

		if (materialIndices.getSize() == 0)
		{
			// The material is not used in the mesh. Nothing is lost in this case so we just skip it.
			continue;
		}

		hkxMaterial* sectMat = HK_NULL;
		if (m_options.m_exportMaterials)
		{
			sectMat = createMaterial(matIds[curMat], triMesh, scene);
		}

		// Vertex buffer
		hkxVertexBuffer* newVB = new hkxVertexBuffer();
		hkxIndexBuffer* newIB = new hkxIndexBuffer();
		fillBuffers(triMesh, meshNode, newVB, newIB, skinControlPointWeights, skinIndicesToClusters, materialIndices);

		hkxMeshSection* newSection = new hkxMeshSection();
		newSection->m_material = sectMat;
		newSection->m_vertexBuffer = newVB;
		newSection->m_indexBuffers.setSize(1);
		newSection->m_indexBuffers[0] = newIB;
		exportedSections.pushBack(newSection);

		if (sectMat)
		{
			sectMat->removeReference();
		}
		newVB->removeReference();
		newIB->removeReference();
	}

	// Create new mesh
	newMesh = new hkxMesh();
	newMesh->m_sections.setSize(exportedSections.getSize());
	for(int cs = 0; cs < newMesh->m_sections.getSize(); ++cs)
	{
		newMesh->m_sections[cs] = exportedSections[cs];
		exportedSections[cs]->removeReference();
	}


	// Add skin bindings
	if (lSkinCount > 0)
	{
		newSkin = new hkxSkinBinding();
		newSkin->m_mesh = newMesh;

		const int lClusterCount = skin->GetClusterCount();
		newSkin->m_bindPose.setSize(lClusterCount);
		newSkin->m_nodeNames.setSize(lClusterCount);

		// Extract bind pose transforms & bone names
		for(int curClusterIndex = 0; curClusterIndex < lClusterCount; ++curClusterIndex)
		{
			FbxCluster* lCluster = skin->GetCluster(curClusterIndex);

			newSkin->m_nodeNames[curClusterIndex] = lCluster->GetLink()->GetName();

			const FbxAMatrix lMatrix = getGlobalPosition(lCluster->GetLink(), m_startTime, m_pose, NULL);			
			convertFbxXMatrixToMatrix4(lMatrix, newSkin->m_bindPose[curClusterIndex]);
		}

		// Extract the world transform of the original, skinned mesh
		{
			FbxAMatrix lMatrix = meshNode->EvaluateGlobalTransform();
			convertFbxXMatrixToMatrix4(lMatrix, newSkin->m_initSkinTransform);
		}
	}


	if (newMesh)
	{
		if (newSkin)
		{
			node->m_object = newSkin;

			scene->m_meshes.pushBack(newMesh);
			scene->m_skinBindings.pushBack(newSkin);
			newMesh->removeReference();
			newSkin->removeReference();
		}
		else
		{
			node->m_object = newMesh;

			scene->m_meshes.pushBack(newMesh);
			newMesh->removeReference();
		}
	}
}

static hkUint8 quantizeWeight(hkReal a_weight)
{
	return static_cast<hkUint8>(hkMath::clamp(a_weight, 1.0f, 0.0f) * 255.0f);
}

static void quantizeWeights(const hkReal inputWeights[4], hkUint8 (&quantizedWeights)[4])
{
	quantizedWeights[0] = quantizeWeight(inputWeights[0]);
	quantizedWeights[1] = quantizeWeight(inputWeights[1]);
	quantizedWeights[2] = quantizeWeight(inputWeights[2]);
	quantizedWeights[3] = quantizeWeight(inputWeights[3]);
}

static bool isNodeFlipped(const FbxNode* node)
{
	if (node == NULL)
		return false;

	FbxDouble3 scaling = node->LclScaling.Get();
	bool flipped = (scaling[0] * scaling[1] * scaling[2]) < 0;
	return flipped != isNodeFlipped(node->GetParent());
}

void FbxToHkxConverter::fillBuffers(
	FbxMesh* pMesh,
	FbxNode* originalNode,
	hkxVertexBuffer* newVB,
	hkxIndexBuffer* newIB,
	const hkArray<float>& skinControlPointWeights,
	const hkArray<int>& skinIndicesToClusters,
	const hkArray<int>& polyIndices)
{
	const int lPolygonCount = polyIndices.getSize();
	// Vertex buffer
	{
		hkxVertexDescription desiredVertDesc;

		desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_POSITION, hkxVertexDescription::HKX_DT_FLOAT, 3)); 

		if (pMesh->GetElementNormal(0)!=NULL)
		{
			desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_NORMAL, hkxVertexDescription::HKX_DT_FLOAT, 3));
		}

		if (pMesh->GetElementVertexColor(0)!=NULL)
		{
			desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_COLOR, hkxVertexDescription::HKX_DT_UINT32, 1));
		}

		for (int c = 0, numUVs = pMesh->GetElementUVCount(); c < numUVs; ++c)
		{
			desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_TEXCOORD, hkxVertexDescription::HKX_DT_FLOAT, 2));
		}

		if (skinControlPointWeights.getSize()>0 && skinIndicesToClusters.getSize()>0)
		{
			desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_BLENDWEIGHTS, hkxVertexDescription::HKX_DT_UINT8, 4));
			desiredVertDesc.m_decls.pushBack(hkxVertexDescription::ElementDecl(hkxVertexDescription::HKX_DU_BLENDINDICES, hkxVertexDescription::HKX_DT_UINT8, 4)); 
		}

		FbxAMatrix geometricTransform;
		{
			FbxNode* meshNode = originalNode;
			FbxVector4 T = meshNode->GetGeometricTranslation(FbxNode::eSourcePivot);
			FbxVector4 R = meshNode->GetGeometricRotation(FbxNode::eSourcePivot);
			FbxVector4 S = meshNode->GetGeometricScaling(FbxNode::eSourcePivot);
			geometricTransform.SetTRS(T,R,S);
		}
		
		// XXX be safe, set the maximum possible vertex num... assuming triangle lists
		const int numVertices = lPolygonCount*3; 
		newVB->setNumVertices(numVertices, desiredVertDesc);

		const hkxVertexDescription& vertDesc = newVB->getVertexDesc();
		const hkxVertexDescription::ElementDecl* posDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_POSITION, 0);
		const hkxVertexDescription::ElementDecl* normDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_NORMAL, 0);
		const hkxVertexDescription::ElementDecl* colorDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_COLOR, 0);
		const hkxVertexDescription::ElementDecl* weightsDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_BLENDWEIGHTS, 0);
		const hkxVertexDescription::ElementDecl* indicesDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_BLENDINDICES, 0);

		const int posStride = posDecl? posDecl->m_byteStride : 0;
		const int normStride = normDecl? normDecl->m_byteStride : 0;
		const int colorStride = colorDecl? colorDecl->m_byteStride : 0;
		const int weightsStride = weightsDecl? weightsDecl->m_byteStride : 0;
		const int indicesStride = indicesDecl? indicesDecl->m_byteStride : 0;

		char* posBuf = static_cast<char*>(posDecl? newVB->getVertexDataPtr(*posDecl): HK_NULL);
		char* normBuf = static_cast<char*>(normDecl? newVB->getVertexDataPtr(*normDecl): HK_NULL);
		char* colorBuf = static_cast<char*>(colorDecl? newVB->getVertexDataPtr(*colorDecl): HK_NULL);
		char* weightsBuf = static_cast<char*>(weightsDecl? newVB->getVertexDataPtr(*weightsDecl): HK_NULL);
		char* indicesBuf = static_cast<char*>(indicesDecl? newVB->getVertexDataPtr(*indicesDecl): HK_NULL);

		const int maxNumUVs = (int) hkxMaterial::PROPERTY_MTL_UV_ID_STAGE_MAX - (int) hkxMaterial::PROPERTY_MTL_UV_ID_STAGE0;
		hkArray<int>::Temp textureCoordinateArrayPositions(maxNumUVs);
		textureCoordinateArrayPositions.setSize(maxNumUVs);
		hkString::memSet4(textureCoordinateArrayPositions.begin(), 0, textureCoordinateArrayPositions.getSize());

		FbxVector4* lControlPoints = pMesh->GetControlPoints();
		int vertexId = 0;

		
		for (int polyIdx = 0; polyIdx < lPolygonCount; polyIdx++)
		{
			// Indirection from the polyIndices for the current material to the mesh's polygon list.
			int i = polyIndices[polyIdx];

			const int lPolygonSize = pMesh->GetPolygonSize(i);
			HK_ASSERT(0x0,lPolygonSize==3);

			for (int j = 0; j < lPolygonSize; j++)
			{
				vertexId = i * 3 + j;
				const int lControlPointIndex = pMesh->GetPolygonVertex(i, j);
				
				if (posBuf)
				{
					FbxVector4 fbxPos = lControlPoints[lControlPointIndex];
					fbxPos = geometricTransform.MultT(fbxPos);

					float* _pos = (float*)(posBuf);
					_pos[0] = (float)fbxPos[0];
					_pos[1] = (float)fbxPos[1];
					_pos[2] = (float)fbxPos[2];
					_pos[3] = 0;
					posBuf += posStride;
				}

				if (normBuf)
				{
					FbxVector4 fbxNormal;
					FbxGeometryElementNormal* leNormal = pMesh->GetElementNormal(0);

					const FbxGeometryElement::EMappingMode mappingMode = leNormal->GetMappingMode();

					if (mappingMode == FbxGeometryElement::eByPolygonVertex)
					{
						switch(leNormal->GetReferenceMode())
						{
						case FbxGeometryElement::eDirect:
							fbxNormal = leNormal->GetDirectArray().GetAt(vertexId);
							break;
						case FbxGeometryElement::eIndexToDirect:
							{
								int id = leNormal->GetIndexArray().GetAt(vertexId);
								fbxNormal = leNormal->GetDirectArray().GetAt(id);
							}
							break;
						default:
							// Other reference modes not shown here!
							break;
						}
					}
					else if (mappingMode == FbxGeometryElement::eByControlPoint)
					{
						switch(leNormal->GetReferenceMode())
						{
						case FbxGeometryElement::eDirect:
							fbxNormal = leNormal->GetDirectArray().GetAt(lControlPointIndex);
							break;
						case FbxGeometryElement::eIndexToDirect:
							{
								int id = leNormal->GetIndexArray().GetAt(lControlPointIndex);
								fbxNormal = leNormal->GetDirectArray().GetAt(id);
							}
							break;
						default:
							// Other reference modes not shown here!
							break;
						}
					}

					float* _normal =(float*)(normBuf);
					_normal[0] = (float)fbxNormal[0];
					_normal[1] = (float)fbxNormal[1];
					_normal[2] = (float)fbxNormal[2];
					_normal[3] = 0;
					normBuf += normStride;
				}

				FbxStringList lUVSetNameList;
				pMesh->GetUVSetNames(lUVSetNameList);

				// Tex coord UV channels
				for(int t = 0, numUVs = hkMath::min2(lUVSetNameList.GetCount(), maxNumUVs); t < numUVs; ++t)
				{
					const char* lUVSetName = lUVSetNameList.GetStringAt(t);
					const FbxGeometryElementUV* leUV = pMesh->GetElementUV(lUVSetName);
					const hkxVertexDescription::ElementDecl* texDecl = vertDesc.getElementDecl(hkxVertexDescription::HKX_DU_TEXCOORD, t);
					HK_ASSERT(0x0, leUV && texDecl);

					const int texCoordStride = texDecl->m_byteStride;
					char* texCoordBuf = static_cast<char*>(newVB->getVertexDataPtr(*texDecl));
					FbxVector2 fbxUV;
 
					switch (leUV->GetMappingMode())
					{
					case FbxGeometryElement::eByControlPoint:
						switch(leUV->GetReferenceMode())
						{
						case FbxGeometryElement::eDirect:
							fbxUV = leUV->GetDirectArray().GetAt(lControlPointIndex);
							break;
						case FbxGeometryElement::eIndexToDirect:
							{
								int id = leUV->GetIndexArray().GetAt(lControlPointIndex);
								fbxUV = leUV->GetDirectArray().GetAt(id);
							}
							break;
						default:
							// Other reference modes not shown here!
							break;
						}
						break;
 
					case FbxGeometryElement::eByPolygonVertex:
						{
							// UV indices are monotonously increasing with each vertex in each polygon.
							int lTextureUVIndex = i * 3 + j; // All polygons have 3 vertices.
							switch(leUV->GetReferenceMode())
							{
							case FbxGeometryElement::eDirect:
								{
									fbxUV = leUV->GetDirectArray().GetAt(lTextureUVIndex);
								}
							case FbxGeometryElement::eIndexToDirect:
								{
									int id = leUV->GetIndexArray().GetAt(lTextureUVIndex);
									fbxUV = leUV->GetDirectArray().GetAt(id);
								}
								break;
							default:
								// Other reference modes not shown here!
								break;
							}
						}
						break;
 
					case FbxGeometryElement::eByPolygon: // Doesn't make much sense for UVs
					case FbxGeometryElement::eAllSame:   // Doesn't make much sense for UVs
					case FbxGeometryElement::eNone:       // Doesn't make much sense for UVs
						break;
					}

					float* _uv =(float*)(texCoordBuf + textureCoordinateArrayPositions[t]);
					_uv[0] = (float)fbxUV[0];
					_uv[1] = (float)fbxUV[1];
					textureCoordinateArrayPositions[t] += texCoordStride;
				}

				if (colorBuf)
				{
					FbxGeometryElementVertexColor* leVtxc = pMesh->GetElementVertexColor(0);
					FbxColor fbxColor;
 
					if (leVtxc != NULL)
					{
						switch(leVtxc->GetMappingMode())
						{
						case FbxGeometryElement::eByControlPoint:
							switch(leVtxc->GetReferenceMode())
							{
							case FbxGeometryElement::eDirect:
								fbxColor = leVtxc->GetDirectArray().GetAt(lControlPointIndex);
								break;
							case FbxGeometryElement::eIndexToDirect:
								{
									int id = leVtxc->GetIndexArray().GetAt(lControlPointIndex);
									fbxColor = leVtxc->GetDirectArray().GetAt(id);
								}
								break;
							default:
								// Other reference modes not shown here!
								break;
							}
							break;
	 
						case FbxGeometryElement::eByPolygonVertex:
							{
								switch(leVtxc->GetReferenceMode())
								{
								case FbxGeometryElement::eDirect:
									fbxColor = leVtxc->GetDirectArray().GetAt(vertexId);
									break;
								case FbxGeometryElement::eIndexToDirect:
									{
										int id = leVtxc->GetIndexArray().GetAt(vertexId);
										fbxColor = leVtxc->GetDirectArray().GetAt(id);
									}
									break;
								default:
									// Other reference modes not shown here!
									break;
								}
							}
							break;
	 
						case FbxGeometryElement::eByPolygon: // Doesn't make much sense for UVs
						case FbxGeometryElement::eAllSame:   // Doesn't make much sense for UVs
						case FbxGeometryElement::eNone:      // Doesn't make much sense for UVs
							break;
						}
					}

					// Vertex color
					unsigned color = elementsToARGB(fbxColor.mRed, fbxColor.mGreen, fbxColor.mBlue, fbxColor.mAlpha); 

					hkUint32* _color = (hkUint32*)(colorBuf);
					*_color = color;

					colorBuf += colorStride;
				}

				if (weightsBuf && indicesBuf)
				{
					const int controlPointFour = lControlPointIndex * 4;
					
					// Add skin indices
					{
						unsigned int compressedI =  unsigned int(skinIndicesToClusters[controlPointFour]  )<< 24 | 
													unsigned int(skinIndicesToClusters[controlPointFour+1])<< 16 | 
													unsigned int(skinIndicesToClusters[controlPointFour+2])<< 8  | 
													unsigned int(skinIndicesToClusters[controlPointFour+3]);

						hkUint32* curIndexBuf =(hkUint32*)(indicesBuf);
						*curIndexBuf = compressedI;
					}
					
					// Add skin weights
					{
						unsigned int compressedW = 0;
						{
							hkReal tempWeights[4];
							for(int i=0; i<4; i++)
							{
								tempWeights[i] = skinControlPointWeights[controlPointFour+i];
							}
	
							hkUint8 tempQWeights[4];
							quantizeWeights(tempWeights, tempQWeights);
	
							compressedW =	unsigned int(tempQWeights[0])<< 24 |
											unsigned int(tempQWeights[1])<< 16 | 
											unsigned int(tempQWeights[2])<< 8  | 
											unsigned int(tempQWeights[3]);
						}

						hkUint32* _w =(hkUint32*)(weightsBuf);
						*_w = compressedW;
					}					

					weightsBuf += weightsStride;
					indicesBuf += indicesStride;
				}				
	
				vertexId++;
			} // For polygonSize
		} // For polygonCount
	}

	// Index buffer... assumes triangle list for now
	{
		newIB->m_indexType = hkxIndexBuffer::INDEX_TYPE_TRI_LIST;
		newIB->m_vertexBaseOffset = 0;
		newIB->m_length = lPolygonCount * 3;
		newIB->m_indices32.setSize(newIB->m_length);

		hkUint32* curIndex = newIB->m_indices32.begin();
		for (int polygonIndex = 0, vertexId = 0; polygonIndex < lPolygonCount; polygonIndex++)
		{
			for (int vertexIndex = 0; vertexIndex < 3; vertexIndex++)
			{
				*curIndex = vertexId++;
				curIndex++;
			}
		}

		// Mirrored meshes need to have their faces flipped (EXP-2773)
		//if (isNodeFlipped(originalNode))
		//{
		//	hkxSceneUtils::flipWinding(*newIB);
		//}
	}
}

template<typename T> static T* getFbxTexture(FbxProperty& materialProperty)
{
	const int textureCount = materialProperty.GetSrcObjectCount<T>();
	if (textureCount > 0)
	{
		if (textureCount > 1)
		{
			HK_WARN(0x0, "More than 1 " << T::ClassId.GetName()<< " found... using first one.");
		}
		return materialProperty.GetSrcObject<T>(0);
	}
	return HK_NULL;
}

hkReferencedObject* FbxToHkxConverter::convertTexture(
	hkxScene *scene,
	FbxSurfaceMaterial* material,
	const FbxStringList& uvSetNames,
	hkxMaterial* mat,
	const char* fbxTextureType,
	int& uvSetIndex)
{
	FbxProperty lProperty = material->FindProperty(fbxTextureType);
	FbxFileTexture* fbxTextureFile = getFbxTexture<FbxFileTexture>(lProperty);
	if (fbxTextureFile)
	{
		// Determine the UV set index to which this texture is associated... fall back to the 0th index in case it can't be found
		{
			const char* str = fbxTextureFile->UVSet.Get().Buffer();
			FbxStringListItem listItem(str);
			uvSetIndex = uvSetNames.Find(listItem);
			const int maxNumUVs = (int) hkxMaterial::PROPERTY_MTL_UV_ID_STAGE_MAX - (int) hkxMaterial::PROPERTY_MTL_UV_ID_STAGE0;
			if (uvSetIndex < 0 || uvSetIndex >= maxNumUVs)
			{
				uvSetIndex = 0;
			}
		}

		hkPointerMap<FbxTexture*, hkRefVariant*>::Iterator it = m_convertedTextures.findKey(fbxTextureFile);
		if (m_convertedTextures.isValid(it))
		{
			hkReferencedObject* texture = m_convertedTextures.getValue(it)->val();
			texture->addReference();
			return texture;
		}
		else
		{
			hkxTextureFile* textureFile = new hkxTextureFile;
			{
				textureFile->m_name = fbxTextureFile->GetName();
				textureFile->m_originalFilename = textureFile->m_filename = fbxTextureFile->GetFileName();
			}
			hkRefVariant* convertedData = new hkRefVariant(textureFile, &textureFile->staticClass());
			m_convertedTextures.insert(fbxTextureFile, convertedData);
			scene->m_externalTextures.pushBack(textureFile);
			return textureFile;
		}
	}

	if (getFbxTexture<FbxLayeredTexture>(lProperty)|| getFbxTexture<FbxProceduralTexture>(lProperty))
	{
		HK_WARN(0x0, "Encountered unsupported texture type - only file textures are currently supported.");
	}

	return HK_NULL;
}

void FbxToHkxConverter::convertTexture(hkxScene *scene, FbxSurfaceMaterial* fbxMat, const FbxStringList& uvSetNames, hkxMaterial* mat, const char* textureTypeName, hkxMaterial::TextureType textureType)
{
	int uvSetIndex = 0;
	hkReferencedObject* convertedTexture = convertTexture(scene, fbxMat, uvSetNames, mat, textureTypeName, uvSetIndex);
	if (convertedTexture)
	{
		hkxMaterial::TextureStage& stage = mat->m_stages.expandOne();
		stage.m_texture = convertedTexture;
		convertedTexture->removeReference();
		stage.m_usageHint = textureType;
		stage.m_tcoordChannel = uvSetIndex;
	}
}

void FbxToHkxConverter::convertTextures(hkxScene *scene, FbxSurfaceMaterial* fbxMat, const FbxStringList& uvSetNames, hkxMaterial* mat)
{
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sDiffuse, hkxMaterial::TEX_DIFFUSE);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sSpecular, hkxMaterial::TEX_SPECULAR);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sEmissive, hkxMaterial::TEX_EMISSIVE);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sBump, hkxMaterial::TEX_BUMP);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sDisplacementFactor, hkxMaterial::TEX_DISPLACEMENT);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sNormalMap, hkxMaterial::TEX_NORMAL);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sReflection, hkxMaterial::TEX_REFLECTION);
	convertTexture(scene, fbxMat, uvSetNames, mat, FbxSurfaceMaterial::sTransparencyFactor, hkxMaterial::TEX_OPACITY);
}

void FbxToHkxConverter::getMaterialsInMesh(FbxMesh* pMesh, hkArray<FbxSurfaceMaterial*>& materialsOut)
{
	FbxNode* lNode = pMesh->GetNode();
	int lMaterialCount = 0;
	if (lNode)
	{
		lMaterialCount = lNode->GetMaterialCount();
	}

	for (int materialIdx = 0; materialIdx < lMaterialCount; ++materialIdx)
	{
		FbxSurfaceMaterial* lMaterial = lNode->GetMaterial(materialIdx);
		materialsOut.pushBack(lMaterial);
	}
}

hkxMaterial* FbxToHkxConverter::createMaterial(FbxSurfaceMaterial* lMaterial, FbxMesh* pMesh, hkxScene* scene)
{
	hkxMaterial* mat = HK_NULL;
	{
		// Test whether this material has already been created.
		auto iterator = m_convertedMaterials.findKey(lMaterial);
		if (m_convertedMaterials.isValid(iterator))
		{
			mat = m_convertedMaterials.getValue(iterator);
			mat->addReference();
			return mat;
		}
	}
	
	if (lMaterial)
	{
		mat = createDefaultMaterial(lMaterial->GetName());
		m_convertedMaterials.insert(lMaterial, mat);

		if (lMaterial->GetClassId().Is(FbxSurfacePhong::ClassId))
		{
			FbxSurfacePhong* phongMaterial = (FbxSurfacePhong *)lMaterial;

			const float transparency =  1.0f - static_cast<float>(phongMaterial->TransparencyFactor.Get());

			convertPropertyToVector4(phongMaterial->Ambient, mat->m_ambientColor);
			convertPropertyToVector4(phongMaterial->Diffuse, mat->m_diffuseColor, transparency);
			convertPropertyToVector4(phongMaterial->Specular, mat->m_specularColor, transparency);
			convertPropertyToVector4(phongMaterial->Emissive, mat->m_emissiveColor);
		}
		else if (lMaterial->GetClassId().Is(FbxSurfaceLambert::ClassId))
		{
			FbxSurfaceLambert* lamberMaterial = (FbxSurfaceLambert *)lMaterial;

			const float transparency = static_cast<float>( lamberMaterial->TransparencyFactor.Get() );

			convertPropertyToVector4(lamberMaterial->Ambient, mat->m_ambientColor);
			convertPropertyToVector4(lamberMaterial->Diffuse, mat->m_diffuseColor, transparency);			
			convertPropertyToVector4(lamberMaterial->Emissive, mat->m_emissiveColor);
		}
		else
		{
			// TODO: create from shaders
			HK_WARN(0x0, "Material \"" << mat->m_name << "\" is of an unsupported type. Expecting Phong or Lambert.");
			lMaterial = 0;
		}

		// Extract texture stage info
		{
			// Get all UV set names from the mesh
			FbxStringList lUVSetNameList;
			pMesh->GetUVSetNames(lUVSetNameList);

			convertTextures(scene, lMaterial, lUVSetNameList, mat);
		}
	}
	else
	{
		mat = createDefaultMaterial("Dummy");
		m_convertedMaterials.insert(lMaterial, mat);
	}
	
	scene->m_materials.pushBack(mat);
	return mat;
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
