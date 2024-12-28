# Allegro

[[_TOC_]]

## 简介

- Allegro是一款高性能大规模播放蒙皮动画的虚幻插件，通过GPU instancing来批量渲染SkinnedMeshRenderer；
- 项目原版的方案是基于虚幻animToTexture基础上开发的，在角色（人马和武器为单位）数量超过2000后，只剩下20帧左右；使用Allegro以后，2000角色稳定在60-70帧以上，上万人规模也有42帧以上；
- 该方案的性能以及功能经过真实项目实践验证，通过分析调研市面上现有的解决方案，结合项目实际数据优化，支持多种功能的同时，又保持了高性能运行效率；



## 主要功能

- GPU instance 蒙皮（skeletal mesh可共享骨架）
- 支持播放AnimSequence，Montage和BlendSpace等动画资源
- 支持动态过渡。平滑地从动画A过渡到B。
- 支持挂载；动态挂载物和静态挂载物；
- 支持submesh 子mesh，可实现Avatar换装换部件功能；
- 适配TAA后处理，没有抗锯齿和动态模糊伪影，准确的计算velocity buffer
- 支持LOD + 视锥裁剪；
- 支持阴影bias 优化，使用更简单的模型绘制阴影；
- 支持PerInstanceCustomDataFloat；
- 支持PerInstance Stencil，设置stencil值；
- 支持动画事件Notify；






## 特性

- 整体上功能齐全，性能优异；其他插件基本上只支持播放简单的AS动画；
- 采用GPU Instance 合批，渲染性能高；
- CPU 端Instance 状态更新采用多任务线程并发，减少GameThread占用
- cpu按照屏占比分级均匀更新动画，提高性能
- 继承MeshComponent，定制VertexFactory Proxy,不继承HISM，和InstancStaticMesh解耦，更独立，更简洁，更灵活定制扩展。
- 不同SKM共用相同动画时，可以共用显存中的动画数据；
- 动画数据不是贴图存储，不受传统的AnimToTexture方案的限制，如，贴图尺寸限制；预生成贴图是静态资源不可变；
- 材质上不需侵入材质处理WPO；计算更少使用更方便；
- 基础功能不需要更改虚幻引擎源码；










## 引擎适配

目前各个引擎版本适配情况如下：

| 引擎版本 | 5.3  |
| :------: | :--: |
| 支持情况  |  ✔️  |

**注意事项：**

该插件支持了一个特殊功能支持PerInstance Stencil，允许设置单个stencil的值，打开此功能需要更改引擎源码；#define ALLEGRO_USE_STENCIL 0 默认为关闭

```diff
--- a/Engine/Source/Runtime/Renderer/Private/CustomDepthRendering.cpp
+++ b/Engine/Source/Runtime/RendererPrivate/CustomDepthRendering.cpp
@@ 529,5 +529,5 @@ 
void FCustomDepthPassMeshProcessor::AddMeshBatch

-const uint32 CustomDepthStencilValue = PrimitiveSceneProxy->GetCustomDepthStencilValue();
+const uint32 CustomDepthStencilValue = MeshBatch.Stencil >=0 ? MeshBatch.Stencil : PrimitiveSceneProxy->GetCustomDepthStencilValue();
  ```

  ```diff
  --- a/Engine/Source/Runtime/Engine/Public/MeshBatch.h
  +++ b/Engine/Source/Runtime/Engine/Public/MeshBatch.h
  @@ 183,5 +183,5 @@ 
  struct FMeshBatch
  
  +int32 Stencil{ -1 };
```











## 开始使用
1. 下载当前仓库到 `Engine/Plugins` 或 `YourProject/Plugins` 目录

2. 开启编辑器，点击【Edit】-【Plugin】窗口勾选本插件:

   <div align=center>
       ![Plugin](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/a757e31de8af72b1df6397f23cf31fda/Plugin.png)
   </div>

3. 根据提示重新编译并重启编辑器，在 【Place Actors】窗口搜索 `Allegro Actor` 如果有结果说明插件成功启用：

   <div align=center>
       ![PlaceActor](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/7216c219be0944fb257cb5a25abf95d4/PlaceActor.png)
   </div>







## 使用流程

Allegro使用时，第一步先创建资源配置文件，第二步如何使用此资源； 下面以蓝图为例，分别展示如何使用其功能；




### 创建Animcollection资源

1. 空白处右键 `Miscellangeous--DataAsset`；选择`Allegro Anim Collection`

   <div align=center>
       ![DataAsset](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/90892e0b8d5c480e15b40b6aaf9dae58/DataAsset.png)
   </div>

 <div align=center>
       ![AllegroClass](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/16642cb7f1eaa346b47566c8e2ea642f/AllegroClass.png)
   </div>


2. 打开新建的AC，先拖拽骨骼Skeleton资源

<div align=center>
       ![AC_Create](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/6155112a3b59d0e69d8427990b60362d/AC_Create.png)
   </div>

  

- 可选择自动全部添加，即点击`Add All Aimations` 和`Add All Meshes` 按钮后`rebuild`，所有Skeleton关联的动画和骨骼会自动添加进去
- 可以手动添加，即把Mesh或者动画资源拖拽进去，再点击`rebuild`



### 蓝图实例说明

这里例举几种常用用法 更多的实例可以参考Content\Test\Manny\AC_Res目录下的蓝图实例

#### 增删实例
1. 新建AllegroActor，先调用SetAnimCollectionAndSkeletalMesh接口设置AC，SKM和Custom参数；

<div align=center>
       ![AllegroBP](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/c91939d46bd1d3d62e52266e54cdceec/AllegroBP.jpg)
   </div>

2. 调用addinstance接口增加实例

<div align=center>
       ![AllegroBP](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/fb02439fc8b471a48c6ea69c5f7e4823/BP_addinstance.png)
   </div>



#### 使用挂载 

1. 在AC中设置需要绑定的骨骼

<div align=center>
       ![BoneToCache](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/b14d68b8076f6145520b4ece66b132f1/BoneToCache.jpg)
   </div>

2. 在蓝图中先Regist注册，在AddAttachment



<div align=center>
       ![BP_Attachment](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/7ba4c06c12818ba5d56abc7534dd320b/BP_Attachment.png)
   </div>





#### 设置customData 
1. 先设置customData位数，再对应设置数据；

<div align=center>
       ![BP_submesh](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/8805f5d73ce385f404b416b21e6a0b4e/BP_submesh.png)
   </div>

2. 材质之中通过GetPerInstanceCustomDataByIndex取出设置的值

<div align=center>
       ![GetCustomdata](http://git-internal.nie.netease.com/wupingyu/allegro/-/wikis/uploads/9d972bfadf3a0c35f464ff871deb965b/GetCustomdata.jpg)
   </div>



## 插件目录
插件中会带有一个 Content 目录，Test 文件夹下存放了一些测试资源和蓝图案例；
