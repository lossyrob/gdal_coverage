# This file is generated by the OGR_API_SPY mechanism.
from osgeo import ogr
from osgeo import osr
import os
import shutil

ogr.Open('non_existing', update = 0)
ogr.Open('non_existing', update = 1)
ds1 = ogr.GetDriverByName('CSV').CreateDataSource('/vsimem/test.csv', options = ['GEOMETRY=AS_WKT'])
ds1_lyr1 = ds1.CreateLayer('test', srs = None, geom_type = ogr.wkbUnknown, options = [])
geom_fd = ogr.GeomFieldDefn('geomfield', ogr.wkbPolygon)
geom_fd.SetSpatialRef(osr.SpatialReference("""GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]]"""))
ds1_lyr1.CreateGeomField(geom_fd, approx_ok = 1)
ds1 = None
ds1 = ogr.Open('/vsimem/test.csv', update = 1)
ds1 = None
ogr.GetDriverByName('CSV').DeleteDataSource('/vsimem/test.csv')
ds1 = ogr.GetDriverByName('ESRI Shapefile').CreateDataSource('/vsimem/test', options = [])
ds1_lyr1 = ds1.CreateLayer('test', srs = osr.SpatialReference("""GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]]"""), geom_type = ogr.wkbPoint, options = ['ENCODING=UTF-8', 'ENCODING=UTF-8'])
ds1_lyr1_defn = ds1_lyr1.GetLayerDefn()
f = ogr.Feature(ds1_lyr1_defn)
ds1_lyr1.CreateFeature(f)
f = None
fd = ogr.FieldDefn('intfield', ogr.OFTInteger)
ds1_lyr1.CreateField(fd, approx_ok = 1)
fd = ogr.FieldDefn('realfield', ogr.OFTReal)
fd.SetWidth(24)
fd.SetPrecision(15)
ds1_lyr1.CreateField(fd, approx_ok = 1)
fd = ogr.FieldDefn('strfield', ogr.OFTString)
ds1_lyr1.CreateField(fd, approx_ok = 1)
ds1_lyr1_defn = ds1_lyr1.GetLayerDefn()
f = ogr.Feature(ds1_lyr1_defn)
f.SetField(0, 1)
f.SetField(1, 2.34)
f.SetField(2, 'bla')
ds1_lyr1.CreateFeature(f)
f = None
f = ogr.Feature(ds1_lyr1_defn)
f.SetFID(1)
f.SetField(0, 1)
f.SetField(2, 'bla')
f.SetGeomField(0, ogr.CreateGeometryFromWkt('POINT (1 2)'))
f.SetStyleString('foo')
ds1_lyr1.SetFeature(f)
f = None
ds1_lyr1.DeleteFeature(1)
ds1_lyr1.ReorderField(0, 2)
ds1_lyr1_defn = ds1_lyr1.GetLayerDefn()
ds1_lyr1.ReorderFields([2, 1, 0])
ds1_lyr1_defn = ds1_lyr1.GetLayerDefn()
ds1_lyr1.DeleteField(1)
fd = ogr.FieldDefn('foo', ogr.OFTString)
ds1_lyr1.AlterFieldDefn(0, fd, 65535)
ds1_lyr1.StartTransaction()
ds1_lyr1.CommitTransaction()
ds1_lyr1.RollbackTransaction()
ds1_lyr1.FindFieldIndex('foo', 1)
ds1_lyr1.GetFeatureCount(force = 1)
ds1_lyr1.GetExtent(geom_field = 0, force = 0)
ds1_lyr1.GetExtent(geom_field = 0, force = 1)
ds1_lyr1.GetSpatialRef()
ds1_lyr1.TestCapability('FastFeatureCount')
ds1_lyr1.GetSpatialFilter()
ds1_lyr1.SetAttributeFilter('foo = \'2\'')
ds1_lyr1.SetAttributeFilter(None)
ds1_lyr1.ResetReading()
ds1_lyr1.GetFeature(0)
ds1_lyr1.GetNextFeature()
ds1_lyr1.SetNextByIndex(0)
for i in range(3):
    ds1_lyr1.GetNextFeature()
ds1_lyr1.SyncToDisk()
ds1_lyr1.GetFIDColumn()
ds1_lyr1.GetGeometryColumn()
ds1_lyr1.GetName()
ds1_lyr1.GetGeomType()
ds1_lyr1.SetIgnoredFields([])
ds1_lyr1.SetSpatialFilter(None)
ds1_lyr1.SetSpatialFilter(0, None)
ds1_lyr1.SetSpatialFilter(ogr.CreateGeometryFromWkt('POINT (1 2)'))
ds1_lyr1.SetSpatialFilterRect(0, 1, 2, 3)
ds1_lyr1.SetSpatialFilterRect(0, 0, 1, 2, 3)
ds1.GetLayerCount()
ds1_lyr1 = ds1.GetLayer(0)
ds1_lyr1 = ds1.GetLayerByName('test')
ds1.GetLayerByName('foo')
ds1_lyr2 = ds1.ExecuteSQL('SELECT * FROM test', None, '')
ds1.ReleaseResultSet(ds1_lyr2)
ds1_lyr2 = ds1.ExecuteSQL('SELECT * FROM test', ogr.CreateGeometryFromWkt('POINT (1 2)'), 'OGRSQL')
ds1.ReleaseResultSet(ds1_lyr2)
ds1.ReleaseResultSet(None)
ds1_lyr2 = ds1.CreateLayer('foo', srs = None, geom_type = ogr.wkbUnknown, options = [])
ds1.DeleteLayer(1)
ds1 = None
ogr.GetDriverByName('ESRI Shapefile').DeleteDataSource('/vsimem/test')
