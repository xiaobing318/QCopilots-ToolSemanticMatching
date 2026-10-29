#!/usr/bin/env python3
"""Regenerate the reviewed small GIS retrieval corpus from explicit facts.

This does not execute QGIS or discover the developer's project. Source checks
make stale IDs visible. English summaries below are authored paraphrases, not
copied QGIS help text. Queries and labels are reviewed independently of scores.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

SOURCE_COMMIT = "8f513743d20f78e88688e2174c59f6dc6a922b2c"

# Native ID | source basename | authored English name | authored description
NATIVE = """
buffer|buffer|Buffer vector features|Create polygon areas within a chosen distance of points, lines or polygons. Distance, end caps, joins and optional dissolving control the result.
clip|clip|Clip vector features by polygon|Keep only parts of vector input features that fall inside a polygon overlay. The output keeps input attributes.
dissolve|dissolve|Dissolve polygon boundaries|Combine geometries, optionally grouping by attribute fields, and remove boundaries between adjacent or overlapping features in each group.
intersection|intersection|Intersect vector layers|Calculate the overlapping geometry of two vector layers and include attributes from both inputs.
union|union|Union vector overlays|Split two polygon layers into overlapping and non-overlapping parts and combine their attribute fields.
difference|difference|Subtract vector overlay|Remove the parts of input geometries covered by an overlay and keep the remainder of the input.
reprojectlayer|transform|Reproject a vector layer|Transform feature coordinates from their source coordinate reference system into a target CRS and produce a new layer.
assignprojection|assignprojection|Assign a vector CRS|Change the CRS declaration for a vector layer without transforming its numeric coordinates. Use when the original CRS metadata is missing or incorrect.
fixgeometries|fixgeometries|Repair invalid geometry|Attempt to construct valid geometries from invalid vector features, such as self-intersecting polygon rings.
checkvalidity|checkvalidity|Check geometry validity|Separate valid and invalid vector geometries and report their geometry errors without treating the diagnostic operation as a repair.
multiparttosingleparts|multiparttosinglepart|Split multipart features|Create individual single-part features from multipart vector geometries while preserving the source attributes.
centroids|centroid|Calculate geometric centroids|Create a point at the geometric center of each vector feature. A centroid is not guaranteed to lie inside a concave polygon.
pointonsurface|pointonsurface|Create interior representative points|Generate a representative point guaranteed to lie on the feature surface, useful when polygon centroids would fall outside.
simplifygeometries|simplify|Simplify vector vertices|Reduce the number of vertices in lines or polygons according to a geometric tolerance while retaining the broad shape.
smoothgeometry|smooth|Smooth vector corners|Round sharp corners in line or polygon features using a smoothing process with iterations and offset controls.
extractbylocation|extractbylocation|Extract features by spatial relationship|Write a subset of input features that satisfy a spatial predicate against another layer, such as within, intersects or disjoint.
extractbyattribute|extractbyattribute|Extract features by attribute|Write a subset of features whose selected attribute satisfies an operator and comparison value.
joinattributesbylocation|joinbylocation|Spatial attribute join|Attach attribute values from spatially matching features in another vector layer using geometric predicates.
joinbynearest|joinbynearest|Join attributes from nearest features|Find the closest features in another vector layer and transfer their attributes, with limits on neighbor count and distance.
mergevectorlayers|mergevector|Merge vector layers|Append features from several compatible vector layers into one output layer and reconcile their fields.
fieldcalculator|fieldcalculator|Calculate an attribute field|Create or update an output attribute using a QGIS expression such as area calculations or arithmetic on existing fields.
polygonize|polygonize|Build polygons from linework|Construct polygon faces from connected vector lines. The source is line geometry rather than raster pixels.
rastercalc|rastercalculator|Raster expression calculator|Compute a new raster from expressions combining raster bands, constants and mathematical or logical operators.
rastersampling|rastersampling|Sample raster values at points|Read raster band values at point feature locations and attach the sampled values as point attributes.
rasterlayerstatistics|rasterstatistics|Raster layer statistics|Compute numeric summaries of a raster band, such as minimum, maximum, mean and standard deviation.
dropgeometries|dropgeometry|Remove geometry from a table|Produce an attribute-only table from vector features by removing the geometry while keeping attribute data.
deletecolumn|dropfields|Delete attribute fields|Remove selected columns from a vector layer while retaining its features and other fields.
convexhull|convexhull|Convex hull of features|Construct the smallest convex geometry enclosing the input geometry, like stretching an elastic band around it.
boundingboxes|boundingbox|Axis-aligned feature bounds|Create rectangular bounding polygons aligned with the coordinate axes for the input features.
""".strip()

GDAL = """
warpreproject|warp|Warp and reproject a raster|Transform raster pixels into another CRS or resolution using a resampling method and target extent, NoData and output options.
assignprojection|AssignProjection|Assign raster projection metadata|Write a raster CRS declaration without resampling or transforming the pixel grid.
cliprasterbymasklayer|ClipRasterByMask|Clip raster using polygon mask|Crop or mask a raster with polygon boundaries, optionally changing resolution and assigning NoData outside the mask.
cliprasterbyextent|ClipRasterByExtent|Clip raster using rectangular extent|Extract a raster window specified by a bounding rectangle rather than an irregular polygon mask.
translate|translate|Convert raster format and options|Translate raster data to another format or data type, with band selection and output creation options such as compression.
rasterize|rasterize|Burn vector features into raster|Create a pixel grid from vector geometry using an attribute or fixed burn value and a selected cell size or image dimension.
polygonize|polygonize|Convert raster regions into vector polygons|Create vector polygons around connected raster cells sharing the same pixel value and store the value as an attribute.
contour|contour|Generate elevation contour lines|Create vector isolines from a raster elevation surface at a chosen vertical interval.
hillshade|hillshade|Shaded relief from elevation|Render terrain shading from a digital elevation model using a simulated sun azimuth and altitude.
slope|slope|Terrain slope|Calculate terrain steepness from a digital elevation raster in degrees or percent.
aspect|aspect|Terrain aspect|Calculate the compass direction faced by terrain slopes from an elevation raster.
fillnodata|fillnodata|Interpolate raster NoData gaps|Fill missing raster cells by interpolation from surrounding valid pixels within a search distance.
merge|merge|Mosaic raster datasets|Combine neighboring or overlapping raster files into a single raster, with optional separate bands and NoData handling.
proximity|proximity|Raster proximity distance|Calculate the distance from raster cells to specified target pixel values and produce a distance raster.
buildvirtualraster|buildvrt|Build virtual raster mosaic|Create a lightweight VRT referencing multiple raster sources without copying every pixel into a new physical mosaic.
gdalinfo|gdalinfo|Inspect raster dataset information|Read raster driver, dimensions, georeferencing, bands and metadata for diagnostic inspection.
""".strip()

TOOLS = """
get_project_crs|interactive_layer_tools.py|Read project CRS|Report the coordinate reference system used by the current QGIS project.
set_project_crs|interactive_layer_tools.py|Set project CRS|Change the QGIS project display coordinate reference system from an authority identifier or WKT.
get_layer_metadata|interactive_layer_tools.py|Inspect map layer metadata|Read a layer's geometry or raster type, CRS, extent, fields and type-specific metadata.
list_map_layers|interactive_layer_tools.py|List loaded project layers|Enumerate the layers currently present in the QGIS project.
load_map_layer|interactive_layer_tools.py|Load a map data source|Open a map layer from a file, provider URI, tile endpoint or supported service and optionally add it to the current project.
query_vector_features|interactive_layer_tools.py|Read vector feature rows|Fetch a bounded page of feature attributes and optionally geometry from a vector layer.
set_vector_selection|interactive_layer_tools.py|Change selected vector features|Replace, add to, remove from or clear a vector layer selection without creating a new dataset.
get_raster_statistics|interactive_layer_tools.py|Read raster band statistics|Calculate bounded statistics for a band of an existing QGIS raster layer.
apply_layer_style|interactive_layer_tools.py|Apply a saved layer style|Load a QML or SLD style from a local file and apply it to a QGIS layer.
configure_vector_labels|interactive_layer_tools.py|Configure vector labels|Enable or disable labels based on a field or expression for a vector layer.
""".strip()

BINARIES = """
bin.gdalwarp|gdalwarp command|Execute GDAL raster warping and resampling with exact argv. Supports target CRS, output dimensions, Lanczos interpolation, NoData, output type and GeoTIFF compression creation options.
bin.gdal_translate|gdal_translate command|Execute GDAL raster translation with exact argv for format conversion, selected bands, output type and creation options.
bin.ogr2ogr|ogr2ogr command|Execute GDAL vector dataset conversion with exact argv, including output format, coordinate transformations, feature filtering and attribute selection.
""".strip()

# Query text is frozen before running a retrieval implementation. Two labels may
# be equally valid only when the stated task truly accepts both entry points.
QUERIES = [
    ("zh", "为道路生成两侧各五百米的缓冲区", ["native:buffer"]),
    ("en", "Create a setback polygon around each school point", ["native:buffer"]),
    ("zh", "按县界裁剪道路矢量，只保留县内的线段", ["native:clip"]),
    ("en", "Trim a vector layer to the county polygon boundary", ["native:clip"]),
    ("zh", "按乡镇名称合并地块并消除内部边界", ["native:dissolve"]),
    ("en", "Dissolve touching parcels by their district field", ["native:dissolve"]),
    ("zh", "获取保护区和地块的重叠区域，并保留两边的属性", ["native:intersection"]),
    ("en", "Intersect two polygon layers and carry attributes from both", ["native:intersection"]),
    ("zh", "把两组面做联合叠加，重叠和不重叠部分都保留", ["native:union"]),
    ("en", "Overlay union retaining the non-overlap of both polygon layers", ["native:union"]),
    ("zh", "从开发区范围中扣除自然保护区", ["native:difference"]),
    ("en", "Erase the overlay polygons from the input parcels", ["native:difference"]),
    ("zh", "把矢量图层的坐标真正转换到 EPSG:3857", ["native:reprojectlayer"]),
    ("en", "Reproject vector feature coordinates into a different CRS", ["native:reprojectlayer"]),
    ("zh", "这份矢量缺少坐标系声明，补写 CRS 但不要改变坐标值", ["native:assignprojection"]),
    ("en", "Assign a CRS label to a vector dataset without coordinate transformation", ["native:assignprojection"]),
    ("zh", "修复面图层里的自相交和无效几何", ["native:fixgeometries"]),
    ("en", "Repair self-intersecting polygon geometries", ["native:fixgeometries"]),
    ("zh", "只检查几何是否有效并输出错误报告，不要修复", ["native:checkvalidity"]),
    ("en", "Diagnose invalid vector geometries and separate errors", ["native:checkvalidity"]),
    ("zh", "把多部件面拆成各自独立的单部件要素", ["native:multiparttosingleparts"]),
    ("en", "Explode multipart polygons into single-part features", ["native:multiparttosingleparts"]),
    ("zh", "计算每个面的几何质心，即使它落在面外也可以", ["native:centroids"]),
    ("en", "Calculate geometric centroids for polygon features", ["native:centroids"]),
    ("zh", "每个凹多边形取一个代表点，必须确保在面内部", ["native:pointonsurface"]),
    ("en", "Place a representative point guaranteed to lie inside each polygon", ["native:pointonsurface"]),
    ("zh", "按容差减少道路折线的顶点数量", ["native:simplifygeometries"]),
    ("en", "Reduce line vertex count using a distance tolerance", ["native:simplifygeometries"]),
    ("zh", "让边界尖角圆滑些，使用多次迭代平滑", ["native:smoothgeometry"]),
    ("en", "Smooth sharp corners of polygon boundaries", ["native:smoothgeometry"]),
    ("zh", "把行政区内的点提取成新的矢量数据集", ["native:extractbylocation"]),
    ("en", "Extract features meeting a spatial predicate into a new layer", ["native:extractbylocation"]),
    ("zh", "提取人口字段超过十万的行政区形成新图层", ["native:extractbyattribute"]),
    ("en", "Write features whose population attribute exceeds a threshold", ["native:extractbyattribute"]),
    ("zh", "根据空间位置把所属行政区属性连接到点图层", ["native:joinattributesbylocation"]),
    ("en", "Transfer polygon attributes to spatially matching point features", ["native:joinattributesbylocation"]),
    ("zh", "给每个村庄附上距离最近医院的属性", ["native:joinbynearest"]),
    ("en", "Join attributes of the closest facility to each feature", ["native:joinbynearest"]),
    ("zh", "将多个同类型道路图层追加合并成一个图层", ["native:mergevectorlayers"]),
    ("en", "Append several vector datasets into a single layer", ["native:mergevectorlayers"]),
    ("zh", "利用表达式新增一个面积属性字段", ["native:fieldcalculator"]),
    ("en", "Calculate a new attribute column using a QGIS expression", ["native:fieldcalculator"]),
    ("zh", "把封闭的矢量线网构造成面", ["native:polygonize"]),
    ("en", "Build polygon faces from connected vector linework", ["native:polygonize"]),
    ("mixed", "用 raster band 表达式计算 NDVI 新栅格", ["native:rastercalc"]),
    ("mixed", "在监测点位置 sample DEM 的像元值到属性表", ["native:rastersampling"]),
    ("zh", "将影像重投影并使用双线性重采样", ["gdal:warpreproject"]),
    ("en", "Warp a raster into another projection with bilinear resampling", ["gdal:warpreproject"]),
    ("zh", "只设置影像的投影元数据，不改动像元网格", ["gdal:assignprojection"]),
    ("en", "Assign raster projection metadata without resampling pixels", ["gdal:assignprojection"]),
    ("zh", "用不规则流域多边形作为掩膜裁剪 DEM", ["gdal:cliprasterbymasklayer"]),
    ("en", "Crop an elevation raster to an irregular watershed mask polygon", ["gdal:cliprasterbymasklayer"]),
    ("zh", "根据给定矩形范围裁剪栅格窗口", ["gdal:cliprasterbyextent"]),
    ("en", "Extract raster pixels inside a rectangular bounding extent", ["gdal:cliprasterbyextent"]),
    ("mixed", "把分类 raster 的相同像元区域转为 vector polygons", ["gdal:polygonize"]),
    ("mixed", "把土地利用面按 category 字段 burn 到栅格网格", ["gdal:rasterize"]),
    ("zh", "从数字高程模型生成每十米一条等高线", ["gdal:contour"]),
    ("en", "Generate contour isolines at ten-meter elevation intervals", ["gdal:contour"]),
    ("zh", "根据太阳方位角和高度角制作地形阴影", ["gdal:hillshade"]),
    ("en", "Render shaded relief from a DEM with a chosen sun angle", ["gdal:hillshade"]),
    ("zh", "根据 DEM 计算坡度，结果以度为单位", ["gdal:slope"]),
    ("en", "Calculate terrain slope steepness in degrees", ["gdal:slope"]),
    ("zh", "计算山坡朝向的罗盘方位", ["gdal:aspect"]),
    ("en", "Determine the compass direction faced by terrain slopes", ["gdal:aspect"]),
    ("zh", "用周围有效像元插值填补栅格 NoData 空洞", ["gdal:fillnodata"]),
    ("en", "Interpolate missing raster cells from nearby valid pixels", ["gdal:fillnodata"]),
    ("mixed", "构建 VRT 虚拟镶嵌，不复制所有影像像元", ["gdal:buildvirtualraster"]),
    ("mixed", "用 gdalwarp 的 argv 做 Lanczos 重采样，输出 Byte、GTiff、DEFLATE 压缩", ["bin.gdalwarp"]),
    ("zh", "读取当前 QGIS 工程使用的坐标参考系统", ["get_project_crs"]),
    ("en", "What coordinate reference system is the current project using?", ["get_project_crs"]),
    ("zh", "列出工程已经加载的地图图层", ["list_map_layers"]),
    ("en", "List the layers currently loaded in this QGIS project", ["list_map_layers"]),
    ("mixed", "把已有 QML style 应用到道路图层", ["apply_layer_style"]),
    ("mixed", "根据 name field 打开矢量图层 labels", ["configure_vector_labels"]),
]


def build(root: Path, destination: Path) -> None:
    records = []
    for provider, rows in (("native", NATIVE), ("gdal", GDAL)):
        for row in rows.splitlines():
            ident, stem, name, description = row.split("|", 3)
            relative = (f"src/analysis/processing/qgsalgorithm{stem}.cpp" if provider == "native"
                        else f"python/plugins/processing/algs/gdal/{stem}.py")
            source = (root / relative).read_text(encoding="utf-8")
            if f'"{ident}"' not in source:
                raise ValueError(f"Cannot verify {provider}:{ident} in {relative}")
            cap = f"{provider}:{ident}"
            records.append({"record_id": cap, "kind": "definition", "capability_id": cap,
                            "name": name, "description": description, "available": True,
                            "payload": {"algorithm_id": cap, "name": name,
                                        "description": description, "provider": provider},
                            "metadata": {"source_path": relative, "source_commit": SOURCE_COMMIT,
                                         "fixture_scope": "retrieval-only-authored-summary"}})
    for row in TOOLS.splitlines():
        ident, basename, name, description = row.split("|", 3)
        relative = f"python/plugins/qcopilots_common/{basename}"
        if f'"{ident}"' not in (root / relative).read_text(encoding="utf-8"):
            raise ValueError(f"Cannot verify {ident} in {relative}")
        records.append({"record_id": ident, "kind": "definition", "capability_id": ident,
                        "name": name, "description": description, "available": True,
                        "payload": {"name": ident, "description": description},
                        "metadata": {"source_path": relative, "source_commit": SOURCE_COMMIT,
                                     "fixture_scope": "retrieval-only-authored-summary"}})
    relative = "python/plugins/qcopilots_mcp_server_qgis_binary/qgis_binaries.json"
    binary_source = (root / relative).read_text(encoding="utf-8")
    for row in BINARIES.splitlines():
        ident, name, description = row.split("|", 2)
        if f'"{ident}"' not in binary_source:
            raise ValueError(f"Cannot verify {ident} in binary catalogue")
        records.append({"record_id": ident, "kind": "definition", "capability_id": ident,
                        "name": name, "description": description, "available": True,
                        "payload": {"binary_id": ident, "name": name, "description": description},
                        "metadata": {"source_path": relative, "source_commit": SOURCE_COMMIT,
                                     "fixture_scope": "retrieval-only-authored-summary"}})
    example = {"binary_id": "bin.gdalwarp", "confirmed_risk": True,
               "client_request_id": "example-only-do-not-execute",
               "arguments": ["--config", "GDAL_PAM_ENABLED", "NO", "-of", "GTiff", "-r", "lanczos",
                             "-ts", "30000", "30000", "-ot", "Byte", "-dstnodata", "0", "-wm", "16",
                             "-wo", "NUM_THREADS=1", "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE",
                             "-co", "ZLEVEL=9", "-co", "NUM_THREADS=1",
                             "C:/GIS/fixtures/input-dem.tif", "C:/GIS/results/warped-output.tif"]}
    records.append({"record_id": "example:gdalwarp-lanczos-geotiff", "kind": "example",
                    "capability_id": "bin.gdalwarp", "name": "Lanczos raster warp example",
                    "description": "A command example requesting Lanczos resampling, Byte output, tiled GeoTIFF and DEFLATE compression.",
                    "payload": example, "available": True,
                    "metadata": {"definition_id": "bin.gdalwarp", "fixture_scope": "example-only-not-executable"}})
    catalog = {"schema_version": 1, "source_id": "qctsm-quality-gis-v1", "revision": "1", "records": records}
    queries = {"schema_version": 1, "dataset_id": "qctsm-gis-multilingual-v1", "revision": "1",
               "source_commit": SOURCE_COMMIT, "top_n": 8, "minimum_recall": 0.90,
               "label_policy": "Every expected ID is relevant. Recall is the fraction of expected capabilities found, averaged over all queries. IDs refer to native algorithm/tool identifiers.",
               "queries": [{"id": f"gis-{n:03d}", "language": lang, "query": query, "expected": expected}
                           for n, (lang, query, expected) in enumerate(QUERIES, 1)]}
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in (("catalog.json", catalog), ("queries.json", queries)):
        (destination / name).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    examples = destination.parents[1] / "examples"
    examples.mkdir(parents=True, exist_ok=True)
    (examples / "gdalwarp-call.json").write_text(json.dumps(example, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    small = dict(catalog)
    small["source_id"] = "gdalwarp-definition-and-example"
    small["records"] = [record for record in records if record["capability_id"] == "bin.gdalwarp"]
    (examples / "gdalwarp-catalog.json").write_text(json.dumps(small, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    manifest = {"schema_version": 1, "catalog_sha256": hashlib.sha256((destination / "catalog.json").read_bytes()).hexdigest(),
                "queries_sha256": hashlib.sha256((destination / "queries.json").read_bytes()).hexdigest(),
                "record_count": len(records), "query_count": len(QUERIES), "source_commit": SOURCE_COMMIT}
    (destination / "fixture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qgis-source", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    build(args.qgis_source, args.output)
