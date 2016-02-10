
/******************************************************************************
 * $Id$
 *
 * Project:  netCDF read/write Driver
 * Purpose:  GDAL bindings over netCDF library.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2004, Frank Warmerdam
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at mines-paris dot org>
 * Copyright (c) 2010, Kyle Shannon <kyle at pobox dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_error.h"
#include "cpl_multiproc.h"
#include "cpl_time.h"
#include "gdal_frmts.h"
#include "netcdfdataset.h"

#include <map> //for NCDFWriteProjAttribs()
#include <limits>

CPL_CVSID("$Id$");

/* Internal function declarations */

static bool NCDFIsGDALVersionGTE(const char* pszVersion, int nTarget);

static void NCDFAddGDALHistory( int fpImage, 
                         const char * pszFilename, const char *pszOldHist,
                         const char * pszFunctionName,
                         const char * pszCFVersion = NCDF_CONVENTIONS_CF_V1_5 );

static void NCDFAddHistory(int fpImage, const char *pszAddHist, const char *pszOldHist);

static bool NCDFIsCfProjection( const char* pszProjection );

static void NCDFWriteProjAttribs(const OGR_SRSNode *poPROJCS,
                            const char* pszProjection,
                            const int fpImage, const int NCDFVarID);

static CPLErr NCDFSafeStrcat(char** ppszDest, const char* pszSrc, size_t* nDestSize);

/* var / attribute helper functions */
static CPLErr NCDFGetAttr( int nCdfId, int nVarId, const char *pszAttrName, 
                    double *pdfValue );
static CPLErr NCDFGetAttr( int nCdfId, int nVarId, const char *pszAttrName, 
                    char **pszValue );
static CPLErr NCDFPutAttr( int nCdfId, int nVarId, 
                    const char *pszAttrName, const char *pszValue );
static CPLErr NCDFGet1DVar( int nCdfId, int nVarId, char **pszValue );//replace this where used
static CPLErr NCDFPut1DVar( int nCdfId, int nVarId, const char *pszValue );

static double NCDFGetDefaultNoDataValue( int nVarType );

/* dimension check functions */
static bool NCDFIsVarLongitude(int nCdfId, int nVarId=-1, const char * nVarName=NULL );
static bool NCDFIsVarLatitude(int nCdfId, int nVarId=-1, const char * nVarName=NULL );
static bool NCDFIsVarProjectionX( int nCdfId, int nVarId=-1, const char * pszVarName=NULL );
static bool NCDFIsVarProjectionY( int nCdfId, int nVarId=-1, const char * pszVarName=NULL );
static bool NCDFIsVarVerticalCoord(int nCdfId, int nVarId=-1, const char * nVarName=NULL );
static bool NCDFIsVarTimeCoord(int nCdfId, int nVarId=-1, const char * nVarName=NULL );

static char **NCDFTokenizeArray( const char *pszValue ); //replace this where used
static void CopyMetadata( void  *poDS, int fpImage, int CDFVarID,
                   const char *pszMatchPrefix=NULL, bool bIsBand=true );

// uncomment this for more debug output
// #define NCDF_DEBUG 1

CPLMutex *hNCMutex = NULL;

/************************************************************************/
/* ==================================================================== */
/*                         netCDFRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class netCDFRasterBand : public GDALPamRasterBand
{
    friend class netCDFDataset;

    nc_type     nc_datatype;
    int         cdfid;
    int         nZId;
    int         nZDim;
    int         nLevel;
    int         nBandXPos;
    int         nBandYPos;
    int         *panBandZPos;
    int         *panBandZLev;
    int         bNoDataSet;
    double      dfNoDataValue;
    double      adfValidRange[2];
    double      dfScale;
    double      dfOffset;
    bool        bSignedData;
    bool        bCheckLongitude;

    CPLErr	    CreateBandMetadata( const int *paDimIds ); 
    template <class T> void CheckData ( void * pImage, 
                                        size_t nTmpBlockXSize, size_t nTmpBlockYSize,
                                        bool bCheckIsNan=false ) ;

  protected:
    CPLXMLNode *SerializeToXML( const char *pszVRTPath );

  public:
    netCDFRasterBand( netCDFDataset *poDS, 
                      int nZId, 
                      int nZDim,
                      int nLevel, 
                      const int *panBandZLen,
                      const int *panBandPos,
                      const int *paDimIds,
                      int nBand );
    netCDFRasterBand( netCDFDataset *poDS, 
                      GDALDataType eType,
                      int nBand,
                      bool bSigned=true,
                      const char *pszBandName=NULL,
                      const char *pszLongName=NULL, 
                      int nZId=-1, 
                      int nZDim=2,
                      int nLevel=0, 
                      const int *panBandZLev=NULL, 
                      const int *panBandZPos=NULL, 
                      const int *paDimIds=NULL );
    ~netCDFRasterBand( );

    virtual double GetNoDataValue( int * );
    virtual CPLErr SetNoDataValue( double );
    //virtual CPLErr DeleteNoDataValue();
    virtual double GetOffset( int * );
    virtual CPLErr SetOffset( double );
    virtual double GetScale( int * );
    virtual CPLErr SetScale( double );
    virtual CPLErr IReadBlock( int, int, void * );
    virtual CPLErr IWriteBlock( int, int, void * );
};

/************************************************************************/
/*                          netCDFRasterBand()                          */
/************************************************************************/

netCDFRasterBand::netCDFRasterBand( netCDFDataset *poNCDFDS, 
                                    int nZIdIn, 
                                    int nZDimIn,
                                    int nLevelIn, 
                                    const int *panBandZLevIn, 
                                    const int *panBandZPosIn, 
                                    const int *paDimIds,
                                    int nBandIn ) :
    cdfid(poNCDFDS->GetCDFID()),
    nBandXPos(panBandZPosIn[0]),
    nBandYPos(panBandZPosIn[1]),
    dfScale(1.0),
    dfOffset(0.0),
    bSignedData(true),   // Default signed, except for Byte.
    bCheckLongitude(false)
{
    this->poDS = poNCDFDS;
    this->panBandZPos = NULL;
    this->panBandZLev = NULL;
    this->nBand = nBandIn;
    this->nZId = nZIdIn;
    this->nZDim = nZDimIn;
    this->nLevel = nLevelIn;

/* ------------------------------------------------------------------- */
/*      Take care of all other dimensions                              */
/* ------------------------------------------------------------------- */
    if( nZDim > 2 ) {
        this->panBandZPos = 
            (int *) CPLCalloc( nZDim-1, sizeof( int ) );
        this->panBandZLev = 
            (int *) CPLCalloc( nZDim-1, sizeof( int ) );

        for ( int i=0; i < nZDim - 2; i++ ){
            this->panBandZPos[i] = panBandZPosIn[i+2];
            this->panBandZLev[i] = panBandZLevIn[i];
        }
    }

    this->dfNoDataValue = 0.0;
    this->bNoDataSet = FALSE;

    nRasterXSize  = poDS->GetRasterXSize( );
    nRasterYSize  = poDS->GetRasterYSize( );
    nBlockXSize   = poDS->GetRasterXSize( );
    nBlockYSize   = 1;

/* -------------------------------------------------------------------- */
/*      Get the type of the "z" variable, our target raster array.      */
/* -------------------------------------------------------------------- */
    if( nc_inq_var( cdfid, nZId, NULL, &nc_datatype, NULL, NULL,
                    NULL ) != NC_NOERR ){
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Error in nc_var_inq() on 'z'." );
        return;
    }

    if( nc_datatype == NC_BYTE )
        eDataType = GDT_Byte;
#ifdef NETCDF_HAS_NC4
    /* NC_UBYTE (unsigned byte) is only available for NC4 */
    else if( nc_datatype == NC_UBYTE )
        eDataType = GDT_Byte;
    else if( nc_datatype == NC_USHORT )
        eDataType = GDT_UInt16;
#endif
    else if( nc_datatype == NC_CHAR )
        eDataType = GDT_Byte;
    else if( nc_datatype == NC_SHORT )
        eDataType = GDT_Int16;
    else if( nc_datatype == NC_INT )
        eDataType = GDT_Int32;
    else if( nc_datatype == NC_FLOAT )
        eDataType = GDT_Float32;
    else if( nc_datatype == NC_DOUBLE )
        eDataType = GDT_Float64;
    else
    {
        if( nBand == 1 )
            CPLError( CE_Warning, CPLE_AppDefined, 
                      "Unsupported netCDF datatype (%d), treat as Float32.", 
                      (int) nc_datatype );
        eDataType = GDT_Float32;
    }

/* -------------------------------------------------------------------- */
/*      Find and set No Data for this variable                          */
/* -------------------------------------------------------------------- */
    nc_type atttype=NC_NAT;
    size_t attlen;
    const char* pszNoValueName = NULL;

    /* find attribute name, either _FillValue or missing_value */
    int status = nc_inq_att( cdfid, nZId, _FillValue, &atttype, &attlen);
    if( status == NC_NOERR ) {
        pszNoValueName = _FillValue;
    }
    else {
        status = nc_inq_att( cdfid, nZId, 
                             "missing_value", &atttype, &attlen );
        if( status == NC_NOERR ) {
            pszNoValueName = "missing_value";
        }
    }

    /* fetch missing value */
    double dfNoData = 0.0;
    bool bGotNoData = false;
    if( status == NC_NOERR ) {
        if ( NCDFGetAttr( cdfid, nZId, pszNoValueName, 
                          &dfNoData ) == CE_None )
        {
            bGotNoData = true;
        }
    }

    /* if NoData was not found, use the default value */
    nc_type vartype=NC_NAT;
    if ( ! bGotNoData ) {
        nc_inq_vartype( cdfid, nZId, &vartype );
        dfNoData = NCDFGetDefaultNoDataValue( vartype );
        /*bGotNoData = true;*/
        CPLDebug( "GDAL_netCDF", 
                  "did not get nodata value for variable #%d, using default %f", 
                  nZId, dfNoData );
    }

/* -------------------------------------------------------------------- */
/*  Look for valid_range or valid_min/valid_max                         */
/* -------------------------------------------------------------------- */
    /* set valid_range to nodata, then check for actual values */
    adfValidRange[0] = dfNoData;
    adfValidRange[1] = dfNoData;
    /* first look for valid_range */
    bool bGotValidRange = false;
    status = nc_inq_att( cdfid, nZId, "valid_range", &atttype, &attlen);
    if( (status == NC_NOERR) && (attlen == 2)) {
        int vrange[2];
        int vmin, vmax;
        status = nc_get_att_int( cdfid, nZId, "valid_range", vrange );
        if( status == NC_NOERR ) {
            bGotValidRange = true;
            adfValidRange[0] = vrange[0];
            adfValidRange[1] = vrange[1];
        }
        /* if not found look for valid_min and valid_max */
        else {
            status = nc_get_att_int( cdfid, nZId, "valid_min", &vmin );
            if( status == NC_NOERR ) {
                adfValidRange[0] = vmin;
                status = nc_get_att_int( cdfid, nZId,
                                         "valid_max", &vmax );
                if( status == NC_NOERR ) {
                    adfValidRange[1] = vmax;
                    bGotValidRange = true;
                }
            }
        }
    }

/* -------------------------------------------------------------------- */
/*  Special For Byte Bands: check for signed/unsigned byte              */
/* -------------------------------------------------------------------- */
    if ( nc_datatype == NC_BYTE ) {

        /* netcdf uses signed byte by default, but GDAL uses unsigned by default */
        /* This may cause unexpected results, but is needed for back-compat */
        if ( poNCDFDS->bIsGdalFile )
            bSignedData = false;
        else
            bSignedData = true;

        /* For NC4 format NC_BYTE is signed, NC_UBYTE is unsigned */
        if ( poNCDFDS->eFormat == NCDF_FORMAT_NC4 ) {
            bSignedData = true;
        }
        else  {
            /* if we got valid_range, test for signed/unsigned range */
            /* http://www.unidata.ucar.edu/software/netcdf/docs/netcdf/Attribute-Conventions.html */
            if ( bGotValidRange ) {
                /* If we got valid_range={0,255}, treat as unsigned */
                if ( (adfValidRange[0] == 0) && (adfValidRange[1] == 255) ) {
                    bSignedData = false;
                    /* reset valid_range */
                    adfValidRange[0] = dfNoData;
                    adfValidRange[1] = dfNoData;
                }
                /* If we got valid_range={-128,127}, treat as signed */
                else if ( (adfValidRange[0] == -128) && (adfValidRange[1] == 127) ) {
                    bSignedData = true;
                    /* reset valid_range */
                    adfValidRange[0] = dfNoData;
                    adfValidRange[1] = dfNoData;
                }
            }
            /* else test for _Unsigned */
            /* http://www.unidata.ucar.edu/software/netcdf/docs/BestPractices.html */
            else {
                char *pszTemp = NULL;
                if ( NCDFGetAttr( cdfid, nZId, "_Unsigned", &pszTemp )
                     == CE_None ) {
                    if ( EQUAL(pszTemp,"true"))
                        bSignedData = false;
                    else if ( EQUAL(pszTemp,"false"))
                        bSignedData = true;
                    CPLFree( pszTemp );
                }
            }
        }

        if ( bSignedData )
        {
            /* set PIXELTYPE=SIGNEDBYTE */
            /* See http://trac.osgeo.org/gdal/wiki/rfc14_imagestructure */
            SetMetadataItem( "PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE" );
        }
        else
        {
            // Fix nodata value as it was stored signed
            if( dfNoData < 0 )
                dfNoData += 256;
        }
    }

#ifdef NETCDF_HAS_NC4
    if ( nc_datatype == NC_UBYTE )
        bSignedData = false;
#endif

    CPLDebug( "GDAL_netCDF", "netcdf type=%d gdal type=%d signedByte=%d",
              nc_datatype, eDataType, static_cast<int>(bSignedData) );

    /* set nodata value */
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "SetNoDataValue(%f) read", dfNoData );
#endif
    SetNoDataValue( dfNoData );

/* -------------------------------------------------------------------- */
/*      Create Band Metadata                                            */
/* -------------------------------------------------------------------- */
    CreateBandMetadata( paDimIds );

/* -------------------------------------------------------------------- */
/* Attempt to fetch the scale_factor and add_offset attributes for the  */
/* variable and set them.  If these values are not available, set       */
/* offset to 0 and scale to 1                                           */
/* -------------------------------------------------------------------- */
    double dfOffset_ = 0.0;
    if ( nc_inq_attid ( cdfid, nZId, CF_ADD_OFFSET, NULL) == NC_NOERR ) {
        status = nc_get_att_double( cdfid, nZId, CF_ADD_OFFSET, &dfOffset_ );
        CPLDebug( "GDAL_netCDF", "got add_offset=%.16g, status=%d", dfOffset_, status );
    }

    double dfScale_ = 1.0;
    if ( nc_inq_attid ( cdfid, nZId,
                        CF_SCALE_FACTOR, NULL) == NC_NOERR ) {
        status = nc_get_att_double( cdfid, nZId, CF_SCALE_FACTOR, &dfScale_ );
        CPLDebug( "GDAL_netCDF", "got scale_factor=%.16g, status=%d", dfScale_, status );
    }
    SetOffset( dfOffset_ );
    SetScale( dfScale_ );

    /* should we check for longitude values > 360 ? */
    bCheckLongitude =
        CPLTestBool(CPLGetConfigOption("GDAL_NETCDF_CENTERLONG_180", "YES"))
        && NCDFIsVarLongitude( cdfid, nZId, NULL );

/* -------------------------------------------------------------------- */
/*      Check for variable chunking (netcdf-4 only)                     */
/*      GDAL block size should be set to hdf5 chunk size                */
/* -------------------------------------------------------------------- */
#ifdef NETCDF_HAS_NC4
    int nTmpFormat = 0;
    size_t chunksize[ MAX_NC_DIMS ];
    status = nc_inq_format( cdfid, &nTmpFormat);
    NetCDFFormatEnum eTmpFormat = static_cast<NetCDFFormatEnum>(nTmpFormat);
    if( ( status == NC_NOERR ) && ( ( eTmpFormat == NCDF_FORMAT_NC4 ) ||
          ( eTmpFormat == NCDF_FORMAT_NC4C ) ) ) {
        /* check for chunksize and set it as the blocksize (optimizes read) */
        status = nc_inq_var_chunking( cdfid, nZId, &nTmpFormat, chunksize );
        if( ( status == NC_NOERR ) && ( nTmpFormat == NC_CHUNKED ) ) {
            CPLDebug( "GDAL_netCDF", 
                      "setting block size to chunk size : %ld x %ld\n",
                      static_cast<long>(chunksize[nZDim-1]), static_cast<long>(chunksize[nZDim-2]));
            nBlockXSize = (int) chunksize[nZDim-1];
            nBlockYSize = (int) chunksize[nZDim-2];
        }
	}
#endif

/* -------------------------------------------------------------------- */
/*      Force block size to 1 scanline for bottom-up datasets if        */
/*      nBlockYSize != 1                                                */
/* -------------------------------------------------------------------- */
    if( poNCDFDS->bBottomUp && nBlockYSize != 1 ) {
        nBlockXSize = nRasterXSize;
        nBlockYSize = 1;
    }
}

/* constructor in create mode */
/* if nZId and following variables are not passed, the band will have 2 dimensions */
/* TODO get metadata, missing val from band #1 if nZDim>2 */
netCDFRasterBand::netCDFRasterBand( netCDFDataset *poNCDFDS,
                                    GDALDataType eType,
                                    int nBandIn,
                                    bool bSigned,
                                    const char *pszBandName,
                                    const char *pszLongName,
                                    int nZIdIn,
                                    int nZDimIn,
                                    int nLevelIn,
                                    const int *panBandZLevIn,
                                    const int *panBandZPosIn,
                                    const int *paDimIds ) :
    nc_datatype(NC_NAT),
    cdfid(poNCDFDS->GetCDFID()),
    nBandXPos(1),
    nBandYPos(0),
    bNoDataSet(FALSE),
    dfNoDataValue(0.0),
    dfScale(0.0),
    dfOffset(0.0),
    bSignedData(bSigned),
    bCheckLongitude(false)
{
    this->poDS = poNCDFDS;
    this->nBand = nBandIn;
    this->nZId = nZIdIn;
    this->nZDim = nZDimIn;
    this->nLevel = nLevelIn;
    this->panBandZPos = NULL;
    this->panBandZLev = NULL;

    nRasterXSize   = poDS->GetRasterXSize( );
    nRasterYSize   = poDS->GetRasterYSize( );
    nBlockXSize   = poDS->GetRasterXSize( );
    nBlockYSize   = 1;

    if ( poDS->GetAccess() != GA_Update ) {
        CPLError( CE_Failure, CPLE_NotSupported, 
                  "Dataset is not in update mode, wrong netCDFRasterBand constructor" );
        return;
    }

/* ------------------------------------------------------------------ */
/*      Take care of all other dimensions                             */
/* ------------------------------------------------------------------ */
    if ( nZDim > 2 && paDimIds != NULL ) {
        nBandXPos = panBandZPosIn[0];
        nBandYPos = panBandZPosIn[1];

        this->panBandZPos = (int *) CPLCalloc( nZDim-1, sizeof( int ) );
        this->panBandZLev = (int *) CPLCalloc( nZDim-1, sizeof( int ) );

        for ( int i=0; i < nZDim - 2; i++ ){
            this->panBandZPos[i] = panBandZPosIn[i+2];
            this->panBandZLev[i] = panBandZLevIn[i];
        }
    }

/* -------------------------------------------------------------------- */
/*      Get the type of the "z" variable, our target raster array.      */
/* -------------------------------------------------------------------- */
    eDataType = eType;

    switch ( eDataType )
    {
        case GDT_Byte:
            nc_datatype = NC_BYTE;
#ifdef NETCDF_HAS_NC4
            /* NC_UBYTE (unsigned byte) is only available for NC4 */
            if ( ! bSignedData && (poNCDFDS->eFormat == NCDF_FORMAT_NC4) )
                nc_datatype = NC_UBYTE;
#endif
            break;
#ifdef NETCDF_HAS_NC4
        // Commented: UInt16 write not supported yet with just that
        //case GDT_UInt16:
        //    nc_datatype = NC_USHORT;
        //    break;
#endif
        case GDT_Int16:
            nc_datatype = NC_SHORT;
            break;
        case GDT_Int32:
            nc_datatype = NC_INT;
            break;
        case GDT_Float32:
            nc_datatype = NC_FLOAT;
            break;
        case GDT_Float64:
            nc_datatype = NC_DOUBLE;
            break;
        default:
            if( nBand == 1 )
                CPLError( CE_Warning, CPLE_AppDefined, 
                          "Unsupported GDAL datatype (%d), treat as NC_FLOAT.", 
                          (int) eDataType );
            nc_datatype = NC_FLOAT;
            break;
    }

/* -------------------------------------------------------------------- */
/*      Define the variable if necessary (if nZId==-1)                  */
/* -------------------------------------------------------------------- */
    bool bDefineVar = false;

    if ( nZId == -1 ) {
        bDefineVar = true;

        /* make sure we are in define mode */
        ( ( netCDFDataset * ) poDS )->SetDefineMode( true );

        char szTempPrivate[256+1];
        const char* pszTemp;
        if ( !pszBandName || EQUAL(pszBandName,"")  )
        {
            snprintf( szTempPrivate, sizeof(szTempPrivate), "Band%d", nBand );
            pszTemp = szTempPrivate;
        }
        else
            pszTemp = pszBandName;

        int status;
        if ( nZDim > 2 && paDimIds != NULL ) {
            status = nc_def_var( cdfid, pszTemp, nc_datatype, 
                                 nZDim, paDimIds, &nZId );
        }
        else {
            int anBandDims[2] = {poNCDFDS->nYDimID, poNCDFDS->nXDimID};
            status = nc_def_var( cdfid, pszTemp, nc_datatype, 
                                 2, anBandDims, &nZId );
        }
        NCDF_ERR(status);
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d) id=%d",
                  cdfid, pszTemp, nc_datatype, nZId );

        if ( !pszLongName || EQUAL(pszLongName,"")  )
        {
            snprintf( szTempPrivate, sizeof(szTempPrivate), "GDAL Band Number %d", nBand );
            pszTemp = szTempPrivate;
        }
        else
            pszTemp = pszLongName;
        status =  nc_put_att_text( cdfid, nZId, CF_LNG_NAME, 
                                   strlen( pszTemp ), pszTemp );
        NCDF_ERR(status);

        poNCDFDS->DefVarDeflate(nZId, true);
    }

    /* for Byte data add signed/unsigned info */
    if ( eDataType == GDT_Byte ) {

        if ( bDefineVar ) { //only add attributes if creating variable
          CPLDebug( "GDAL_netCDF", "adding valid_range attributes for Byte Band" );
          /* For unsigned NC_BYTE (except NC4 format) */
          /* add valid_range and _Unsigned ( defined in CF-1 and NUG ) */
          int status = NC_NOERR;
          if ( (nc_datatype == NC_BYTE) && (poNCDFDS->eFormat != NCDF_FORMAT_NC4) ) {
            short int l_adfValidRange[2]; 
            if  ( bSignedData ) {
                l_adfValidRange[0] = -128;
                l_adfValidRange[1] = 127;
                status = nc_put_att_text( cdfid,nZId, 
                                          "_Unsigned", 5, "false" );
            }
            else {
                l_adfValidRange[0] = 0;
                l_adfValidRange[1] = 255;
                    status = nc_put_att_text( cdfid,nZId, 
                                              "_Unsigned", 4, "true" );
            }
            NCDF_ERR(status);
            status=nc_put_att_short( cdfid,nZId, "valid_range",
                                     NC_SHORT, 2, l_adfValidRange );
            NCDF_ERR(status);
          }
        }
        /* for unsigned byte set PIXELTYPE=SIGNEDBYTE */
        /* See http://trac.osgeo.org/gdal/wiki/rfc14_imagestructure */
        if  ( bSignedData ) 
            SetMetadataItem( "PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE" );

    }

    /* set default nodata */
    double dfNoData = NCDFGetDefaultNoDataValue( nc_datatype );
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "SetNoDataValue(%f) default", dfNoData );
#endif
    SetNoDataValue( dfNoData );
}

/************************************************************************/
/*                         ~netCDFRasterBand()                          */
/************************************************************************/

netCDFRasterBand::~netCDFRasterBand()
{
    FlushCache();
    CPLFree( panBandZPos );
    CPLFree( panBandZLev );
}

/************************************************************************/
/*                             GetOffset()                              */
/************************************************************************/
double netCDFRasterBand::GetOffset( int *pbSuccess ) 
{
    if( pbSuccess != NULL ) 
        *pbSuccess = TRUE; 

    return dfOffset; 
}

/************************************************************************/
/*                             SetOffset()                              */
/************************************************************************/
CPLErr netCDFRasterBand::SetOffset( double dfNewOffset ) 
{
    CPLMutexHolderD(&hNCMutex);

    dfOffset = dfNewOffset; 

    /* write value if in update mode */
    if ( poDS->GetAccess() == GA_Update ) {

        /* make sure we are in define mode */
        ( ( netCDFDataset * ) poDS )->SetDefineMode( true );

        int status = nc_put_att_double( cdfid, nZId, CF_ADD_OFFSET,
                                    NC_DOUBLE, 1, &dfOffset );

        NCDF_ERR(status);
        if ( status == NC_NOERR )
            return CE_None;

        return CE_Failure;
    }

    return CE_None; 
}

/************************************************************************/
/*                              GetScale()                              */
/************************************************************************/
double netCDFRasterBand::GetScale( int *pbSuccess ) 
{
    if( pbSuccess != NULL ) 
        *pbSuccess = TRUE;

    return dfScale; 
}

/************************************************************************/
/*                              SetScale()                              */
/************************************************************************/
CPLErr netCDFRasterBand::SetScale( double dfNewScale )  
{
    CPLMutexHolderD(&hNCMutex);

    dfScale = dfNewScale; 

    /* write value if in update mode */
    if ( poDS->GetAccess() == GA_Update ) {

        /* make sure we are in define mode */
        ( ( netCDFDataset * ) poDS )->SetDefineMode( true );

        int status = nc_put_att_double( cdfid, nZId, CF_SCALE_FACTOR,
                                    NC_DOUBLE, 1, &dfScale );

        NCDF_ERR(status);
        if ( status == NC_NOERR )
            return CE_None;

        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double netCDFRasterBand::GetNoDataValue( int * pbSuccess )

{
    if( pbSuccess )
        *pbSuccess = bNoDataSet;

    if( bNoDataSet )
        return dfNoDataValue;

    return GDALPamRasterBand::GetNoDataValue( pbSuccess );
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr netCDFRasterBand::SetNoDataValue( double dfNoData )

{
    CPLMutexHolderD(&hNCMutex);

    /* If already set to new value, don't do anything */
    if ( bNoDataSet && CPLIsEqual( dfNoData, dfNoDataValue ) )
        return CE_None;

    /* write value if in update mode */
    if ( poDS->GetAccess() == GA_Update ) {

        /* netcdf-4 does not allow to set _FillValue after leaving define mode */
        /* but it's ok if variable has not been written to, so only print debug */
        /* see bug #4484 */
        if ( bNoDataSet && !reinterpret_cast<netCDFDataset *>(poDS)->GetDefineMode() ) {
            CPLDebug( "GDAL_netCDF", 
                      "Setting NoDataValue to %.18g (previously set to %.18g) "
                      "but file is no longer in define mode (id #%d, band #%d)", 
                      dfNoData, dfNoDataValue, cdfid, nBand );
        }
#ifdef NCDF_DEBUG
        else {
            CPLDebug( "GDAL_netCDF", "Setting NoDataValue to %.18g (id #%d, band #%d)", 
                      dfNoData, cdfid, nBand );
        }
#endif
        /* make sure we are in define mode */
        reinterpret_cast<netCDFDataset *>( poDS )->SetDefineMode( true );

        int status;
        if ( eDataType == GDT_Byte) {
            if ( bSignedData ) {
                signed char cNoDataValue = (signed char) dfNoData;
                status = nc_put_att_schar( cdfid, nZId, _FillValue,
                                           nc_datatype, 1, &cNoDataValue );
            }
            else {
                unsigned char ucNoDataValue = (unsigned char) dfNoData;
                status = nc_put_att_uchar( cdfid, nZId, _FillValue,
                                           nc_datatype, 1, &ucNoDataValue );
            }
        }
        else if ( eDataType == GDT_Int16 ) {
            short int nsNoDataValue = (short int) dfNoData;
            status = nc_put_att_short( cdfid, nZId, _FillValue,
                                       nc_datatype, 1, &nsNoDataValue );
        }
        else if ( eDataType == GDT_Int32) {
            int nNoDataValue = (int) dfNoData;
            status = nc_put_att_int( cdfid, nZId, _FillValue,
                                     nc_datatype, 1, &nNoDataValue );
        }
        else if ( eDataType == GDT_Float32) {
            float fNoDataValue = (float) dfNoData;
            status = nc_put_att_float( cdfid, nZId, _FillValue,
                                       nc_datatype, 1, &fNoDataValue );
        }
        else
            status = nc_put_att_double( cdfid, nZId, _FillValue,
                                        nc_datatype, 1, &dfNoData );

        NCDF_ERR(status);

        /* update status if write worked */
        if ( status == NC_NOERR ) {
            dfNoDataValue = dfNoData;
            bNoDataSet = TRUE;
            return CE_None;
        }

        return CE_Failure;

    }

    dfNoDataValue = dfNoData;
    bNoDataSet = TRUE;
    return CE_None;
}

/************************************************************************/
/*                        DeleteNoDataValue()                           */
/************************************************************************/

#ifdef notdef
CPLErr netCDFRasterBand::DeleteNoDataValue()

{
    CPLMutexHolderD(&hNCMutex);

    if ( !bNoDataSet )
        return CE_None;

    /* write value if in update mode */
    if ( poDS->GetAccess() == GA_Update ) {

        /* make sure we are in define mode */
        ( ( netCDFDataset * ) poDS )->SetDefineMode( true );

        status = nc_del_att( cdfid, nZId, _FillValue );

        NCDF_ERR(status);

        /* update status if write worked */
        if ( status == NC_NOERR ) {
            dfNoDataValue = 0.0;
            bNoDataSet = FALSE;
            return CE_None;
        }

        return CE_Failure;
    }

    dfNoDataValue = 0.0;
    bNoDataSet = FALSE;
    return CE_None;
}
#endif

/************************************************************************/
/*                           SerializeToXML()                           */
/************************************************************************/

CPLXMLNode *netCDFRasterBand::SerializeToXML( CPL_UNUSED const char *pszUnused )
{
/* -------------------------------------------------------------------- */
/*      Overridden from GDALPamDataset to add only band histogram        */
/*      and statistics. See bug #4244.                                  */
/* -------------------------------------------------------------------- */
    if( psPam == NULL )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Setup root node and attributes.                                 */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psTree = CPLCreateXMLNode( NULL, CXT_Element, "PAMRasterBand" );

    if( GetBand() > 0 )
    {
        CPLString oFmt;
        CPLSetXMLValue( psTree, "#band", oFmt.Printf( "%d", GetBand() ) );
    }

/* -------------------------------------------------------------------- */
/*      Histograms.                                                     */
/* -------------------------------------------------------------------- */
    if( psPam->psSavedHistograms != NULL )
        CPLAddXMLChild( psTree, CPLCloneXMLTree( psPam->psSavedHistograms ) );

/* -------------------------------------------------------------------- */
/*      Metadata (statistics only).                                     */
/* -------------------------------------------------------------------- */
    GDALMultiDomainMetadata oMDMDStats;
    const char* papszMDStats[] = { "STATISTICS_MINIMUM", "STATISTICS_MAXIMUM",
                                   "STATISTICS_MEAN", "STATISTICS_STDDEV",
                                   NULL };
    for ( int i=0; i<CSLCount((char**)papszMDStats); i++ ) {
        if ( GetMetadataItem( papszMDStats[i] ) != NULL )
            oMDMDStats.SetMetadataItem( papszMDStats[i],
                                       GetMetadataItem(papszMDStats[i]) );
    }
    CPLXMLNode *psMD = oMDMDStats.Serialize();

    if( psMD != NULL )
    {
        if( psMD->psChild == NULL )
            CPLDestroyXMLNode( psMD );
        else
            CPLAddXMLChild( psTree, psMD );
    }

/* -------------------------------------------------------------------- */
/*      We don't want to return anything if we had no metadata to       */
/*      attach.                                                         */
/* -------------------------------------------------------------------- */
    if( psTree->psChild == NULL || psTree->psChild->psNext == NULL )
    {
        CPLDestroyXMLNode( psTree );
        psTree = NULL;
    }

    return psTree;
}

/************************************************************************/
/*                         CreateBandMetadata()                         */
/************************************************************************/

CPLErr netCDFRasterBand::CreateBandMetadata( const int *paDimIds )

{
    netCDFDataset *l_poDS = reinterpret_cast<netCDFDataset *>( this->poDS );

/* -------------------------------------------------------------------- */
/*      Compute all dimensions from Band number and save in Metadata    */
/* -------------------------------------------------------------------- */
    char szVarName[NC_MAX_NAME+1];
    szVarName[0] = '\0';
    int status = nc_inq_varname( cdfid, nZId, szVarName );
    NCDF_ERR(status);

    int nd;
    nc_inq_varndims( cdfid, nZId, &nd );
/* -------------------------------------------------------------------- */
/*      Compute multidimention band position                            */
/*                                                                      */
/* BandPosition = (Total - sum(PastBandLevels) - 1)/sum(remainingLevels)*/
/* if Data[2,3,4,x,y]                                                   */
/*                                                                      */
/*  BandPos0 = (nBand ) / (3*4)                                         */
/*  BandPos1 = (nBand - BandPos0*(3*4) ) / (4)                          */
/*  BandPos2 = (nBand - BandPos0*(3*4) ) % (4)                          */
/* -------------------------------------------------------------------- */

    SetMetadataItem( "NETCDF_VARNAME", szVarName );
    int Sum = 1;
    if( nd == 3 ) {
        Sum *= panBandZLev[0];
    }

/* -------------------------------------------------------------------- */
/*      Loop over non-spatial dimensions                                */
/* -------------------------------------------------------------------- */
    int nVarID = -1;
    int result = 0;
    int Taken = 0;

    for( int i=0; i < nd-2 ; i++ ) {

        if( i != nd - 2 -1 ) {
            Sum = 1;
            for( int j=i+1; j < nd-2; j++ ) {
                Sum *= panBandZLev[j];
            }
            result = (int) ( ( nLevel-Taken) / Sum );
        }
        else {
            result = (int) ( ( nLevel-Taken) % Sum );
        }

        snprintf(szVarName,sizeof(szVarName),"%s",
               l_poDS->papszDimName[paDimIds[panBandZPos[i]]] );

        // TODO: Make sure all the status checks make sense.

        status = nc_inq_varid( cdfid, szVarName, &nVarID );
        if( status != NC_NOERR ) {
            /* Try to uppercase the first letter of the variable */
            /* Note: why is this needed? leaving for safety */
            szVarName[0] = (char) toupper(szVarName[0]);
            /* status = */nc_inq_varid( cdfid, szVarName, &nVarID );
        }

        nc_type nVarType = NC_NAT;
        /* status = */ nc_inq_vartype( cdfid, nVarID, &nVarType );

        int nDims = 0;
        /* status = */ nc_inq_varndims( cdfid, nVarID, &nDims );

        char szMetaTemp[256];
        if( nDims == 1 ) {
            size_t count[1] = {1};
            size_t start[1] = {static_cast<size_t>(result)};

            switch( nVarType ) {
                case NC_SHORT:
                    short sData;
                    /* status = */ nc_get_vara_short( cdfid, nVarID,
                                                 start,
                                                 count, &sData );
                    snprintf( szMetaTemp, sizeof(szMetaTemp), "%d", sData );
                    break;
                case NC_INT:
                    int nData;
                    /* status = */ nc_get_vara_int( cdfid, nVarID,
                                               start,
                                               count, &nData );
                    snprintf( szMetaTemp, sizeof(szMetaTemp), "%d", nData );
                    break;
                case NC_FLOAT:
                    float fData;
                    /* status = */nc_get_vara_float( cdfid, nVarID,
                                                 start,
                                                 count, &fData );
                    CPLsnprintf( szMetaTemp, sizeof(szMetaTemp), "%.8g", fData );
                    break;
                case NC_DOUBLE:
                    double dfData;
                    /* status = */ nc_get_vara_double( cdfid, nVarID,
                                                  start,
                                                  count, &dfData);
                    CPLsnprintf( szMetaTemp, sizeof(szMetaTemp), "%.16g", dfData );
                    break;
                default:
                    CPLDebug( "GDAL_netCDF", "invalid dim %s, type=%d",
                              szMetaTemp, nVarType);
                    break;
            }
        }
        else
            snprintf( szMetaTemp, sizeof(szMetaTemp), "%d", result+1);

/* -------------------------------------------------------------------- */
/*      Save dimension value                                            */
/* -------------------------------------------------------------------- */
        /* NOTE: removed #original_units as not part of CF-1 */

        char szMetaName[NC_MAX_NAME+1+32];
        snprintf( szMetaName, sizeof(szMetaName), "NETCDF_DIM_%s",  szVarName );
        SetMetadataItem( szMetaName, szMetaTemp );

        Taken += result * Sum;

    } // end loop non-spatial dimensions

/* -------------------------------------------------------------------- */
/*      Get all other metadata                                          */
/* -------------------------------------------------------------------- */
    int nAtt=0;
    nc_inq_varnatts( cdfid, nZId, &nAtt );

    for( int i=0; i < nAtt ; i++ ) {

        char szMetaName[NC_MAX_NAME+1];
        szMetaName[0] = 0;
        status = nc_inq_attname( cdfid, nZId, i, szMetaName);
        if ( status != NC_NOERR ) 
            continue;

        char *pszMetaValue = NULL;
        if ( NCDFGetAttr( cdfid, nZId, szMetaName, &pszMetaValue) == CE_None ) {
            SetMetadataItem( szMetaName, pszMetaValue );
        }
        else {
            CPLDebug( "GDAL_netCDF", "invalid Band metadata %s", szMetaName );
        }

        if ( pszMetaValue )  {
            CPLFree( pszMetaValue );
            pszMetaValue = NULL;
        }

    }

    return CE_None;
}

/************************************************************************/
/*                             CheckData()                              */
/************************************************************************/
template <class T>
void  netCDFRasterBand::CheckData ( void * pImage, 
                                    size_t nTmpBlockXSize, size_t nTmpBlockYSize,
                                    bool bCheckIsNan ) 
{
  CPLAssert( pImage != NULL );

  /* if this block is not a full block (in the x axis), we need to re-arrange the data 
     this is because partial blocks are not arranged the same way in netcdf and gdal */
  if ( nTmpBlockXSize != static_cast<size_t>(nBlockXSize) ) {
    T* ptr = (T *) CPLCalloc( nTmpBlockXSize*nTmpBlockYSize, sizeof( T ) );
    memcpy( ptr, pImage, nTmpBlockXSize*nTmpBlockYSize*sizeof( T ) );
    for( size_t j=0; j<nTmpBlockYSize; j++) {
      size_t k = j*nBlockXSize;
      for( size_t i=0; i<nTmpBlockXSize; i++,k++)
        ((T *) pImage)[k] = ptr[j*nTmpBlockXSize+i];
      for( size_t i=nTmpBlockXSize; i<static_cast<size_t>(nBlockXSize); i++,k++)
        ((T *) pImage)[k] = (T)dfNoDataValue;
    }
    CPLFree( ptr );
  }

  /* is valid data checking needed or requested? */
  if ( (adfValidRange[0] != dfNoDataValue) || 
       (adfValidRange[1] != dfNoDataValue) ||
       bCheckIsNan ) {
    for( size_t j=0; j<nTmpBlockYSize; j++) {
      // k moves along the gdal block, skipping the out-of-range pixels
      size_t k = j*nBlockXSize;
      for( size_t i=0; i<nTmpBlockXSize; i++,k++) {
        /* check for nodata and nan */
        if ( CPLIsEqual( (double) ((T *)pImage)[k], dfNoDataValue ) )
          continue;
        if( bCheckIsNan && CPLIsNan( (double) (( (T *) pImage))[k] ) ) { 
          ( (T *)pImage )[k] = (T)dfNoDataValue;
          continue;
        }
        /* check for valid_range */
        if ( ( ( adfValidRange[0] != dfNoDataValue ) && 
               ( ((T *)pImage)[k] < (T)adfValidRange[0] ) ) 
             || 
             ( ( adfValidRange[1] != dfNoDataValue ) && 
               ( ((T *)pImage)[k] > (T)adfValidRange[1] ) ) ) {
          ( (T *)pImage )[k] = (T)dfNoDataValue;
        }
      }
    }
  }

  /* If minimum longitude is > 180, subtract 360 from all.
     If not, disable checking for further calls (check just once).
     Only check first and last block elements since lon must be monotonic. */
  const bool bIsSigned = std::numeric_limits<T>::is_signed;
  if ( bCheckLongitude && bIsSigned &&
       MIN( ((T *)pImage)[0], ((T *)pImage)[nTmpBlockXSize-1] ) > 180.0 ) {
    for( size_t j=0; j<nTmpBlockYSize; j++) {
      size_t k = j*nBlockXSize;
      for( size_t i=0; i<nTmpBlockXSize; i++,k++) {
        if ( ! CPLIsEqual( (double) ((T *)pImage)[k], dfNoDataValue ) )
          ((T *)pImage )[k] = static_cast<T>(((T *)pImage )[k] - 360);
      }
    }
  }
  else
    bCheckLongitude = false;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr netCDFRasterBand::IReadBlock( int nBlockXOff, int nBlockYOff,
                                     void * pImage )

{
    CPLMutexHolderD(&hNCMutex);

    int nd = 0;
    nc_inq_varndims ( cdfid, nZId, &nd );

#ifdef NCDF_DEBUG
    if ( (nBlockYOff == 0) || (nBlockYOff == nRasterYSize-1) )
        CPLDebug( "GDAL_netCDF", "netCDFRasterBand::IReadBlock( %d, %d, ... ) nBand=%d nd=%d",
                  nBlockXOff, nBlockYOff, nBand, nd );
#endif

/* -------------------------------------------------------------------- */
/*      Locate X, Y and Z position in the array                         */
/* -------------------------------------------------------------------- */

    size_t start[ MAX_NC_DIMS ];
    memset( start, 0, sizeof( start ) );
    start[nBandXPos] = nBlockXOff * nBlockXSize;

    // check y order
    if( ( ( netCDFDataset *) poDS )->bBottomUp ) {
#ifdef NCDF_DEBUG
      if ( (nBlockYOff == 0) || (nBlockYOff == nRasterYSize-1) )
        CPLDebug( "GDAL_netCDF", 
                  "reading bottom-up dataset, nBlockYSize=%d nRasterYSize=%d",
                  nBlockYSize, nRasterYSize );
#endif
      // check block size - return error if not 1
      // reading upside-down rasters with nBlockYSize!=1 needs further development
      // perhaps a simple solution is to invert geotransform and not use bottom-up
      if ( nBlockYSize == 1 ) {
        start[nBandYPos] = nRasterYSize - 1 - nBlockYOff;       
      }
      else {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "nBlockYSize = %d, only 1 supported when reading bottom-up dataset", nBlockYSize );
        return CE_Failure;
      }
    } else {
      start[nBandYPos] = nBlockYOff * nBlockYSize;
    }

    size_t edge[ MAX_NC_DIMS ];
    memset( edge,  0, sizeof( edge )  );

    edge[nBandXPos] = nBlockXSize;
    if ( ( start[nBandXPos] + edge[nBandXPos] ) > (size_t)nRasterXSize )
       edge[nBandXPos] = nRasterXSize - start[nBandXPos];
    edge[nBandYPos] = nBlockYSize;
    if ( ( start[nBandYPos] + edge[nBandYPos] ) > (size_t)nRasterYSize )
      edge[nBandYPos] = nRasterYSize - start[nBandYPos];

#ifdef NCDF_DEBUG
    if ( (nBlockYOff == 0) || (nBlockYOff == nRasterYSize-1) )
      CPLDebug( "GDAL_netCDF", "start={%ld,%ld} edge={%ld,%ld} bBottomUp=%d",
                start[nBandXPos], start[nBandYPos], edge[nBandXPos], edge[nBandYPos], 
                ( ( netCDFDataset *) poDS )->bBottomUp );
#endif

    if( nd == 3 ) {
        start[panBandZPos[0]]  = nLevel;     // z
        edge [panBandZPos[0]]  = 1;
    }

/* -------------------------------------------------------------------- */
/*      Compute multidimention band position                            */
/*                                                                      */
/* BandPosition = (Total - sum(PastBandLevels) - 1)/sum(remainingLevels)*/
/* if Data[2,3,4,x,y]                                                   */
/*                                                                      */
/*  BandPos0 = (nBand ) / (3*4)                                         */
/*  BandPos1 = (nBand - (3*4) ) / (4)                                   */
/*  BandPos2 = (nBand - (3*4) ) % (4)                                   */
/* -------------------------------------------------------------------- */
    if (nd > 3) 
    {
        int Sum = -1;
        int Taken = 0;
        for( int i=0; i < nd-2 ; i++ ) 
        {
            if( i != nd - 2 -1 ) {
                Sum = 1;
                for( int j=i+1; j < nd-2; j++ ) {
                    Sum *= panBandZLev[j];
                }
                start[panBandZPos[i]] = (int) ( ( nLevel-Taken) / Sum );
                edge[panBandZPos[i]] = 1;
            } else {
                start[panBandZPos[i]] = (int) ( ( nLevel-Taken) % Sum );
                edge[panBandZPos[i]] = 1;
            }
            Taken += static_cast<int>(start[panBandZPos[i]]) * Sum;
        }
    }

    /* make sure we are in data mode */
    ( ( netCDFDataset * ) poDS )->SetDefineMode( false );

    /* read data according to type */
    int status;
    if( eDataType == GDT_Byte ) 
    {
        if (this->bSignedData) 
        {
            status = nc_get_vara_schar( cdfid, nZId, start, edge, 
                                        (signed char *) pImage );
            if ( status == NC_NOERR ) 
                CheckData<signed char>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                        false );
        }
        else {
            status = nc_get_vara_uchar( cdfid, nZId, start, edge, 
                                        (unsigned char *) pImage );
            if ( status == NC_NOERR ) 
                CheckData<unsigned char>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                          false ); 
        }
    }

    else if( eDataType == GDT_Int16 )
    {
        status = nc_get_vara_short( cdfid, nZId, start, edge, 
                                    (short int *) pImage );
        if ( status == NC_NOERR ) 
            CheckData<short int>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                  false ); 
    }
#ifdef NETCDF_HAS_NC4
    else if( eDataType == GDT_UInt16 )
    {
        status = nc_get_vara_ushort( cdfid, nZId, start, edge, 
                                    (unsigned short int *) pImage );
        if ( status == NC_NOERR ) 
            CheckData<unsigned short int>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                  false ); 
    }
#endif
    else if( eDataType == GDT_Int32 )
    {
        if( sizeof(long) == 4 )
        {
            status = nc_get_vara_long( cdfid, nZId, start, edge, 
                                       (long int *) pImage );
            if ( status == NC_NOERR ) 
                CheckData<long int>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                     false ); 
        }
        else
        {
            status = nc_get_vara_int( cdfid, nZId, start, edge, 
                                      (int *) pImage );
            if ( status == NC_NOERR ) 
                CheckData<int>( pImage, edge[nBandXPos], edge[nBandYPos], 
                                false ); 
        }
    }
    else if( eDataType == GDT_Float32 )
    {
        status = nc_get_vara_float( cdfid, nZId, start, edge, 
                                    (float *) pImage );
        if ( status == NC_NOERR ) 
            CheckData<float>( pImage, edge[nBandXPos], edge[nBandYPos], 
                              true ); 
    }
    else if( eDataType == GDT_Float64 )
    {
        status = nc_get_vara_double( cdfid, nZId, start, edge, 
                                     (double *) pImage ); 
        if ( status == NC_NOERR ) 
            CheckData<double>( pImage, edge[nBandXPos], edge[nBandYPos], 
                               true ); 
    }
    else
        status = NC_EBADTYPE;

    if( status != NC_NOERR )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "netCDF scanline fetch failed: #%d (%s)", 
                  status, nc_strerror( status ) );
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                             IWriteBlock()                            */
/************************************************************************/

CPLErr netCDFRasterBand::IWriteBlock( CPL_UNUSED int nBlockXOff,
                                      int nBlockYOff,
                                      void * pImage )
{
    CPLMutexHolderD(&hNCMutex);

#ifdef NCDF_DEBUG
    if ( (nBlockYOff == 0) || (nBlockYOff == nRasterYSize-1) )
        CPLDebug( "GDAL_netCDF", "netCDFRasterBand::IWriteBlock( %d, %d, ... ) nBand=%d",
                  nBlockXOff, nBlockYOff, nBand );
#endif

    int nd;
    nc_inq_varndims ( cdfid, nZId, &nd );

/* -------------------------------------------------------------------- */
/*      Locate X, Y and Z position in the array                         */
/* -------------------------------------------------------------------- */

    size_t start[ MAX_NC_DIMS];
    memset( start, 0, sizeof( start ) );

    start[nBandXPos] = 0;          // x dim can move around in array
    // check y order
    if( ( ( netCDFDataset *) poDS )->bBottomUp ) {
        start[nBandYPos] = nRasterYSize - 1 - nBlockYOff;
    } else {
        start[nBandYPos] = nBlockYOff; // y
    }

    size_t edge[ MAX_NC_DIMS ];
    memset( edge,  0, sizeof( edge )  );

    edge[nBandXPos] = nBlockXSize; 
    edge[nBandYPos] = 1;

    if( nd == 3 ) {
        start[panBandZPos[0]]  = nLevel;     // z
        edge [panBandZPos[0]]  = 1;
    }

/* -------------------------------------------------------------------- */
/*      Compute multidimention band position                            */
/*                                                                      */
/* BandPosition = (Total - sum(PastBandLevels) - 1)/sum(remainingLevels)*/
/* if Data[2,3,4,x,y]                                                   */
/*                                                                      */
/*  BandPos0 = (nBand ) / (3*4)                                         */
/*  BandPos1 = (nBand - (3*4) ) / (4)                                   */
/*  BandPos2 = (nBand - (3*4) ) % (4)                                   */
/* -------------------------------------------------------------------- */
    if (nd > 3) 
    {
        int Sum = -1;
        int Taken = 0;
        for( int i=0; i < nd-2 ; i++ ) 
        {
            if( i != nd - 2 -1 ) {
                Sum = 1;
                for( int j=i+1; j < nd-2; j++ ) {
                    Sum *= panBandZLev[j];
                }
                start[panBandZPos[i]] = (int) ( ( nLevel-Taken) / Sum );
                edge[panBandZPos[i]] = 1;
            } else {
                start[panBandZPos[i]] = (int) ( ( nLevel-Taken) % Sum );
                edge[panBandZPos[i]] = 1;
            }
            Taken += static_cast<int>(start[panBandZPos[i]]) * Sum;
        }
    }

    /* make sure we are in data mode */
    ( ( netCDFDataset * ) poDS )->SetDefineMode( false );

    /* copy data according to type */
    int status;
    if( eDataType == GDT_Byte ) {
        if ( this->bSignedData ) 
            status = nc_put_vara_schar( cdfid, nZId, start, edge, 
                                         (signed char*) pImage);
        else
            status = nc_put_vara_uchar( cdfid, nZId, start, edge, 
                                         (unsigned char*) pImage);
    }
    else if( ( eDataType == GDT_UInt16 ) || ( eDataType == GDT_Int16 ) ) {
        status = nc_put_vara_short( cdfid, nZId, start, edge, 
                                     (short int *) pImage);
    }
    else if( eDataType == GDT_Int32 ) {
        status = nc_put_vara_int( cdfid, nZId, start, edge, 
                                   (int *) pImage);
    }
    else if( eDataType == GDT_Float32 ) {
        status = nc_put_vara_float( cdfid, nZId, start, edge, 
                                    (float *) pImage);
    }
    else if( eDataType == GDT_Float64 ) {
        status = nc_put_vara_double( cdfid, nZId, start, edge, 
                                     (double *) pImage);
    }
    else {
        CPLError( CE_Failure, CPLE_NotSupported,
                  "The NetCDF driver does not support GDAL data type %d",
                  eDataType );
        status = NC_EBADTYPE;
    }
    NCDF_ERR(status);

    if( status != NC_NOERR )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "netCDF scanline write failed: %s", 
                  nc_strerror( status ) );
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                              netCDFDataset                           */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           netCDFDataset()                            */
/************************************************************************/

netCDFDataset::netCDFDataset() :
    // Basic dataset vars.
    cdfid(-1),
    papszSubDatasets(NULL),
    papszMetadata(NULL),
    bBottomUp(true),
    eFormat(NCDF_FORMAT_NONE),
    bIsGdalFile(false),
    bIsGdalCfFile(false),

    pszCFProjection(NULL),
    pszCFCoordinates(NULL),

    // projection/GT.
    pszProjection(NULL),
    nXDimID(-1),
    nYDimID(-1),
    bIsProjected(false),
    bIsGeographic(false),  // Can be not projected, and also not geographic

    // State vars.
    bDefineMode(true),
    bSetProjection(false),
    bSetGeoTransform(false),
    bAddedProjectionVars(false),
    bAddedGridMappingRef(false),

    // Create vars.
    papszCreationOptions(NULL),
    eCompress(NCDF_COMPRESS_NONE),
    nZLevel(NCDF_DEFLATE_LEVEL),
#ifdef NETCDF_HAS_NC4
    bChunking(false),
#endif
    nCreateMode(NC_CLOBBER),
    bSignedData(true),
    nLayers(0),
    papoLayers(NULL)
{
    /* projection/GT */
    adfGeoTransform[0] = 0.0;
    adfGeoTransform[1] = 1.0;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0;
}

/************************************************************************/
/*                           ~netCDFDataset()                           */
/************************************************************************/

netCDFDataset::~netCDFDataset()

{
    CPLMutexHolderD(&hNCMutex);

    #ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "netCDFDataset::~netCDFDataset(), cdfid=%d filename=%s",
              cdfid, osFilename.c_str() );
    #endif
    /* make sure projection is written if GeoTransform OR Projection are missing */
    if( (GetAccess() == GA_Update) && (! bAddedProjectionVars) ) {
        if ( bSetProjection && ! bSetGeoTransform )
            AddProjectionVars();
        else if ( bSetGeoTransform && ! bSetProjection )
            AddProjectionVars();
            // CPLError( CE_Warning, CPLE_AppDefined, 
            //           "netCDFDataset::~netCDFDataset() Projection was not defined, projection will be missing" );
    }

    FlushCache();

    for(int i=0;i<nLayers;i++)
        delete papoLayers[i];

    /* make sure projection variable is written to band variable */
    if( (GetAccess() == GA_Update) && ! bAddedGridMappingRef )
        AddGridMappingRef();

    CSLDestroy( papszMetadata );
    CSLDestroy( papszSubDatasets );
    CSLDestroy( papszCreationOptions );

    CPLFree( pszProjection );
    CPLFree( pszCFProjection );
    CPLFree( pszCFCoordinates );

    if( cdfid > 0 ) {
#ifdef NCDF_DEBUG
        CPLDebug( "GDAL_netCDF", "calling nc_close( %d )", cdfid );
#endif
        int status = nc_close( cdfid );
        NCDF_ERR(status);
    }
}

/************************************************************************/
/*                            SetDefineMode()                           */
/************************************************************************/
int netCDFDataset::SetDefineMode( bool bNewDefineMode )
{
    /* do nothing if already in new define mode
       or if dataset is in read-only mode */
    if ( ( bDefineMode == bNewDefineMode ) || 
         ( GetAccess() == GA_ReadOnly ) ) 
        return CE_None;

    CPLDebug( "GDAL_netCDF", "SetDefineMode(%d) old=%d",
              static_cast<int>(bNewDefineMode), static_cast<int>(bDefineMode) );

    bDefineMode = bNewDefineMode;

    int status;
    if ( bDefineMode )
        status = nc_redef( cdfid );
    else
        status = nc_enddef( cdfid );

    NCDF_ERR(status);
    return status;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **netCDFDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALDataset::GetMetadataDomainList(),
                                   TRUE,
                                   "SUBDATASETS", NULL);
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/
char **netCDFDataset::GetMetadata( const char *pszDomain )
{
    if( pszDomain != NULL && STARTS_WITH_CI(pszDomain, "SUBDATASETS") )
        return papszSubDatasets;

    return GDALDataset::GetMetadata( pszDomain );
}

/************************************************************************/
/*                          GetProjectionRef()                          */
/************************************************************************/

const char * netCDFDataset::GetProjectionRef()
{
    if( bSetProjection )
        return pszProjection;

    return GDALPamDataset::GetProjectionRef();
}

/************************************************************************/
/*                           SerializeToXML()                           */
/************************************************************************/

CPLXMLNode *netCDFDataset::SerializeToXML( const char *pszUnused )

{
/* -------------------------------------------------------------------- */
/*      Overridden from GDALPamDataset to add only band histogram        */
/*      and statistics. See bug #4244.                                  */
/* -------------------------------------------------------------------- */

    if( psPam == NULL )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Setup root node and attributes.                                 */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psDSTree = CPLCreateXMLNode( NULL, CXT_Element, "PAMDataset" );

/* -------------------------------------------------------------------- */
/*      Process bands.                                                  */
/* -------------------------------------------------------------------- */
    for( int iBand = 0; iBand < GetRasterCount(); iBand++ )
    {
        netCDFRasterBand *poBand = (netCDFRasterBand *) 
            GetRasterBand(iBand+1);

        if( poBand == NULL || !(poBand->GetMOFlags() & GMO_PAM_CLASS) )
            continue;

        CPLXMLNode *psBandTree = poBand->SerializeToXML( pszUnused );

        if( psBandTree != NULL )
            CPLAddXMLChild( psDSTree, psBandTree );
    }

/* -------------------------------------------------------------------- */
/*      We don't want to return anything if we had no metadata to       */
/*      attach.                                                         */
/* -------------------------------------------------------------------- */
    if( psDSTree->psChild == NULL )
    {
        CPLDestroyXMLNode( psDSTree );
        psDSTree = NULL;
    }

    return psDSTree;
}

/************************************************************************/
/*                           FetchCopyParm()                            */
/************************************************************************/

double netCDFDataset::FetchCopyParm( const char *pszGridMappingValue, 
                                     const char *pszParm, double dfDefault )

{
    char szTemp[ 256 ];
    snprintf(szTemp, sizeof(szTemp), "%s#%s", pszGridMappingValue, pszParm);
    const char *pszValue = CSLFetchNameValue(papszMetadata, szTemp);

    if( pszValue )
    {
        return CPLAtofM(pszValue);
    }

    return dfDefault;
}

/************************************************************************/
/*                           FetchStandardParallels()                   */
/************************************************************************/

char** netCDFDataset::FetchStandardParallels( const char *pszGridMappingValue )
{
    char         szTemp[256 ];
    //cf-1.0 tags
    snprintf(szTemp, sizeof(szTemp), "%s#%s", pszGridMappingValue, CF_PP_STD_PARALLEL);
    const char *pszValue = CSLFetchNameValue( papszMetadata, szTemp );

    char **papszValues = NULL;
    if( pszValue != NULL ) {
        papszValues = NCDFTokenizeArray( pszValue );
    }
    //try gdal tags
    else
    {
        snprintf(szTemp, sizeof(szTemp), "%s#%s", pszGridMappingValue, CF_PP_STD_PARALLEL_1);

        pszValue = CSLFetchNameValue( papszMetadata, szTemp );

        if ( pszValue != NULL )
            papszValues = CSLAddString( papszValues, pszValue );

        snprintf(szTemp, sizeof(szTemp), "%s#%s", pszGridMappingValue, CF_PP_STD_PARALLEL_2);

        pszValue = CSLFetchNameValue( papszMetadata, szTemp );

        if( pszValue != NULL )	
            papszValues = CSLAddString( papszValues, pszValue );
    }

    return papszValues;
}

/************************************************************************/
/*                      SetProjectionFromVar()                          */
/************************************************************************/
void netCDFDataset::SetProjectionFromVar( int nVarId, bool bReadSRSOnly )
{
    double       dfStdP1=0.0;
    double       dfStdP2=0.0;
    double       dfCenterLat=0.0;
    double       dfCenterLon=0.0;
    double       dfScale=1.0;
    double       dfFalseEasting=0.0;
    double       dfFalseNorthing=0.0;
    double       dfCentralMeridian=0.0;
    double       dfEarthRadius=0.0;
    double       dfInverseFlattening=0.0;
    double       dfLonPrimeMeridian=0.0;
    const char   *pszPMName=NULL;
    double       dfSemiMajorAxis=0.0;
    double       dfSemiMinorAxis=0.0;

    bool         bGotGeogCS = false;
    bool         bGotCfSRS = false;
    bool         bGotGdalSRS = false;
    bool         bGotCfGT = false;
    bool         bGotGdalGT = false;

    /* These values from CF metadata */
    OGRSpatialReference oSRS;
    double       *pdfXCoord = NULL;
    double       *pdfYCoord = NULL;
    char         szDimNameX[ NC_MAX_NAME+1 ];
    //char         szDimNameY[ NC_MAX_NAME+1 ];
    int          nSpacingBegin=0;
    int          nSpacingMiddle=0;
    int          nSpacingLast=0;
    bool         bLatSpacingOK=false;
    bool         bLonSpacingOK=false;
    size_t       xdim = nRasterXSize;
    size_t       ydim = nRasterYSize;

    const char  *pszUnits = NULL;

    /* These values from GDAL metadata */
    const char *pszWKT = NULL;
    const char *pszGeoTransform = NULL;

    netCDFDataset * poDS = this; /* perhaps this should be removed for clarity */

    CPLDebug( "GDAL_netCDF", "\n=====\nSetProjectionFromVar( %d )\n", nVarId );

/* -------------------------------------------------------------------- */
/*      Get x/y range information.                                      */
/* -------------------------------------------------------------------- */

    /* temp variables to use in SetGeoTransform() and SetProjection() */
    double adfTempGeoTransform[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    char *pszTempProjection = NULL;

    if ( !bReadSRSOnly && (xdim == 1 || ydim == 1) ) {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "1-pixel width/height files not supported, xdim: %ld ydim: %ld",
                  (long)xdim, (long)ydim );
        return;
    }

/* -------------------------------------------------------------------- */
/*      Look for grid_mapping metadata                                  */
/* -------------------------------------------------------------------- */

    char szGridMappingName[ NC_MAX_NAME+1 ];
    strcpy( szGridMappingName, "" );

    char szGridMappingValue[ NC_MAX_NAME+1 ];
    strcpy( szGridMappingValue, "" );

    char szVarName[ NC_MAX_NAME+1 ];
    szVarName[0] = '\0';
    {
    int status = nc_inq_varname( cdfid, nVarId, szVarName );
    NCDF_ERR(status);
    }
    char szTemp[ NC_MAX_NAME+1 ];
    snprintf(szTemp,sizeof(szTemp), "%s#%s", szVarName,CF_GRD_MAPPING);

    const char *pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);
    if( pszValue ) {
        snprintf(szGridMappingName,sizeof(szGridMappingName), "%s", szTemp);
        snprintf(szGridMappingValue,sizeof(szGridMappingValue), "%s", pszValue);
    }

    if( !EQUAL( szGridMappingValue, "" )  ) {

        /*  Read grid_mapping metadata */
        int nVarProjectionID = -1;
        nc_inq_varid( cdfid, szGridMappingValue, &nVarProjectionID );
        poDS->ReadAttributes( cdfid, nVarProjectionID );

/* -------------------------------------------------------------------- */
/*      Look for GDAL spatial_ref and GeoTransform within grid_mapping  */
/* -------------------------------------------------------------------- */
        CPLDebug( "GDAL_netCDF", "got grid_mapping %s", szGridMappingValue );
        snprintf(szTemp,sizeof(szTemp), "%s#%s", szGridMappingValue,NCDF_SPATIAL_REF);

        pszWKT = CSLFetchNameValue(poDS->papszMetadata, szTemp);

        if( pszWKT != NULL ) {
            snprintf(szTemp,sizeof(szTemp), "%s#%s", szGridMappingValue,NCDF_GEOTRANSFORM);
            pszGeoTransform = CSLFetchNameValue(poDS->papszMetadata, szTemp);
        }
    }

/* -------------------------------------------------------------------- */
/*      Get information about the file.                                 */
/* -------------------------------------------------------------------- */
/*      Was this file created by the GDAL netcdf driver?                */
/*      Was this file created by the newer (CF-conformant) driver?      */
/* -------------------------------------------------------------------- */
/* 1) If GDAL netcdf metadata is set, and version >= 1.9,               */
/*    it was created with the new driver                                */
/* 2) Else, if spatial_ref and GeoTransform are present in the          */
/*    grid_mapping variable, it was created by the old driver           */
/* -------------------------------------------------------------------- */
    pszValue = CSLFetchNameValue(poDS->papszMetadata, "NC_GLOBAL#GDAL");

    if( pszValue && NCDFIsGDALVersionGTE(pszValue, 1900)) {
        bIsGdalFile = true;
        bIsGdalCfFile = true;
    }
    else  if( pszWKT != NULL && pszGeoTransform != NULL ) {
        bIsGdalFile = true;
        bIsGdalCfFile = false;
    }

/* -------------------------------------------------------------------- */
/*      Set default bottom-up default value                             */
/*      Y axis dimension and absence of GT can modify this value        */
/*      Override with Config option GDAL_NETCDF_BOTTOMUP                */
/* -------------------------------------------------------------------- */
   /* new driver is bottom-up by default */
   if ( bIsGdalFile && ! bIsGdalCfFile )
       poDS->bBottomUp = false;
   else
       poDS->bBottomUp = true;

    CPLDebug( "GDAL_netCDF", 
              "bIsGdalFile=%d bIsGdalCfFile=%d bBottomUp=%d", 
              static_cast<int>(bIsGdalFile), static_cast<int>(bIsGdalCfFile),
              static_cast<int>(bBottomUp) );

/* -------------------------------------------------------------------- */
/*      Look for dimension: lon                                         */
/* -------------------------------------------------------------------- */

    memset( szDimNameX, '\0', sizeof(szDimNameX) );
    //memset( szDimNameY, '\0', sizeof(szDimNameY) );

    if( !bReadSRSOnly )
    {
        for( unsigned int i = 0; (i < strlen( poDS->papszDimName[ poDS->nXDimID ] )
                                && i < 3 ); i++ ) {
            szDimNameX[i]=(char)tolower( ( poDS->papszDimName[poDS->nXDimID] )[i] );
        }
        szDimNameX[3] = '\0';
        /*for( unsigned int i = 0; (i < strlen( poDS->papszDimName[ poDS->nYDimID ] )
                                && i < 3 ); i++ ) {
            szDimNameY[i]=(char)tolower( ( poDS->papszDimName[poDS->nYDimID] )[i] );
        }
        szDimNameY[3] = '\0';*/
    }

/* -------------------------------------------------------------------- */
/*      Read grid_mapping information and set projections               */
/* -------------------------------------------------------------------- */

    if( !( EQUAL(szGridMappingName,"" ) ) ) {

        snprintf(szTemp,sizeof(szTemp), "%s#%s", szGridMappingValue,CF_GRD_MAPPING_NAME);
        pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);

        if( pszValue != NULL ) {

/* -------------------------------------------------------------------- */
/*      Check for datum/spheroid information                            */
/* -------------------------------------------------------------------- */
            dfEarthRadius = 
                poDS->FetchCopyParm( szGridMappingValue, 
                                     CF_PP_EARTH_RADIUS, 
                                     -1.0 );

            dfLonPrimeMeridian = 
                poDS->FetchCopyParm( szGridMappingValue,
                                     CF_PP_LONG_PRIME_MERIDIAN, 
                                     0.0 );
            // should try to find PM name from its value if not Greenwich
            if ( ! CPLIsEqual(dfLonPrimeMeridian,0.0) )
                pszPMName = "unknown";

            dfInverseFlattening = 
                poDS->FetchCopyParm( szGridMappingValue, 
                                     CF_PP_INVERSE_FLATTENING, 
                                     -1.0 );

            dfSemiMajorAxis = 
                poDS->FetchCopyParm( szGridMappingValue, 
                                     CF_PP_SEMI_MAJOR_AXIS, 
                                     -1.0 );

            dfSemiMinorAxis = 
                poDS->FetchCopyParm( szGridMappingValue, 
                                     CF_PP_SEMI_MINOR_AXIS, 
                                     -1.0 );

            //see if semi-major exists if radius doesn't
            if( dfEarthRadius < 0.0 )
                dfEarthRadius = dfSemiMajorAxis;

            //if still no radius, check old tag
            if( dfEarthRadius < 0.0 )
                dfEarthRadius = poDS->FetchCopyParm( szGridMappingValue, 
                                                     CF_PP_EARTH_RADIUS_OLD,
                                                     -1.0 );

            //has radius value
            if( dfEarthRadius > 0.0 ) {
                //check for inv_flat tag
                if( dfInverseFlattening < 0.0 ) {
                    //no inv_flat tag, check for semi_minor
                    if( dfSemiMinorAxis < 0.0 ) {
                        //no way to get inv_flat, use sphere
                        oSRS.SetGeogCS( "unknown", 
                                        NULL, 
                                        "Sphere", 
                                        dfEarthRadius, 0.0,
                                        pszPMName, dfLonPrimeMeridian );
                        bGotGeogCS = true;
                    }
                    else {
                        if( dfSemiMajorAxis < 0.0 )
                            dfSemiMajorAxis = dfEarthRadius;
                        //set inv_flat using semi_minor/major
                        dfInverseFlattening = OSRCalcInvFlattening(dfSemiMajorAxis, dfSemiMinorAxis);

                        oSRS.SetGeogCS( "unknown", 
                                        NULL, 
                                        "Spheroid", 
                                        dfEarthRadius, dfInverseFlattening,
                                        pszPMName, dfLonPrimeMeridian );
                        bGotGeogCS = true;
                    }
                }
                else {
                    oSRS.SetGeogCS( "unknown", 
                                    NULL, 
                                    "Spheroid", 
                                    dfEarthRadius, dfInverseFlattening,
                                        pszPMName, dfLonPrimeMeridian );
                    bGotGeogCS = true;
                }

                if ( bGotGeogCS )
                    CPLDebug( "GDAL_netCDF", "got spheroid from CF: (%f , %f)", dfEarthRadius, dfInverseFlattening );

            }
            //no radius, set as wgs84 as default?
            else {
                // This would be too indiscriminate.  But we should set
                // it if we know the data is geographic.
                // oSRS.SetWellKnownGeogCS( "WGS84" );
            }

/* -------------------------------------------------------------------- */
/*      Transverse Mercator                                             */
/* -------------------------------------------------------------------- */

            if( EQUAL( pszValue, CF_PT_TM ) ) {

                dfScale = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_SCALE_FACTOR_MERIDIAN, 1.0 );

                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LONG_CENTRAL_MERIDIAN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;
                oSRS.SetTM( dfCenterLat, 
                            dfCenterLon,
                            dfScale,
                            dfFalseEasting,
                            dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );
            }

/* -------------------------------------------------------------------- */
/*      Albers Equal Area                                               */
/* -------------------------------------------------------------------- */

            if( EQUAL( pszValue, CF_PT_AEA ) ) {

                char **papszStdParallels = NULL;

                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LONG_CENTRAL_MERIDIAN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                papszStdParallels = 
                    FetchStandardParallels( szGridMappingValue );

                if( papszStdParallels != NULL ) {

                    if ( CSLCount( papszStdParallels ) == 1 ) {
                        /* TODO CF-1 standard says it allows AEA to be encoded with only 1 standard parallel */
                        /* how should this actually map to a 2StdP OGC WKT version? */
                        CPLError( CE_Warning, CPLE_NotSupported, 
                                  "NetCDF driver import of AEA-1SP is not tested, using identical std. parallels\n" );
                        dfStdP1 = CPLAtofM( papszStdParallels[0] );
                        dfStdP2 = dfStdP1;

                    }
                    else if( CSLCount( papszStdParallels ) == 2 ) {
                        dfStdP1 = CPLAtofM( papszStdParallels[0] );
                        dfStdP2 = CPLAtofM( papszStdParallels[1] );
                    }
                }
                //old default
                else {
                    dfStdP1 = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_STD_PARALLEL_1, 0.0 );

                    dfStdP2 = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_STD_PARALLEL_2, 0.0 );
                }

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                bGotCfSRS = true;
                oSRS.SetACEA( dfStdP1, dfStdP2, dfCenterLat, dfCenterLon,
                              dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

                CSLDestroy( papszStdParallels );
            }

/* -------------------------------------------------------------------- */
/*      Cylindrical Equal Area                                          */
/* -------------------------------------------------------------------- */

            else if( EQUAL( pszValue, CF_PT_CEA ) || EQUAL( pszValue, CF_PT_LCEA ) ) {

                char **papszStdParallels = NULL;

                papszStdParallels = 
                    FetchStandardParallels( szGridMappingValue );

                if( papszStdParallels != NULL ) {
                    dfStdP1 = CPLAtofM( papszStdParallels[0] );
                }
                else {
                    //TODO: add support for 'scale_factor_at_projection_origin' variant to standard parallel
                    //Probably then need to calc a std parallel equivalent
                    CPLError( CE_Failure, CPLE_NotSupported, 
                              "NetCDF driver does not support import of CF-1 LCEA "
                              "'scale_factor_at_projection_origin' variant yet.\n" );
                }

                dfCentralMeridian = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LONG_CENTRAL_MERIDIAN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;
                oSRS.SetCEA( dfStdP1, dfCentralMeridian,
                             dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

                CSLDestroy( papszStdParallels );
            }

/* -------------------------------------------------------------------- */
/*      lambert_azimuthal_equal_area                                    */
/* -------------------------------------------------------------------- */
            else if( EQUAL( pszValue, CF_PT_LAEA ) ) {
                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LON_PROJ_ORIGIN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                oSRS.SetProjCS( "LAEA (WGS84) " );

                bGotCfSRS = true;
                oSRS.SetLAEA( dfCenterLat, dfCenterLon,
                              dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );
            }

/* -------------------------------------------------------------------- */
/*      Azimuthal Equidistant                                           */
/* -------------------------------------------------------------------- */
            else if( EQUAL( pszValue, CF_PT_AE ) ) {
                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LON_PROJ_ORIGIN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;
                oSRS.SetAE( dfCenterLat, dfCenterLon,
                            dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

            }

/* -------------------------------------------------------------------- */
/*      Lambert conformal conic                                         */
/* -------------------------------------------------------------------- */
            else if( EQUAL( pszValue, CF_PT_LCC ) ) {

                char **papszStdParallels = NULL;

                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LONG_CENTRAL_MERIDIAN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                papszStdParallels = 
                    FetchStandardParallels( szGridMappingValue );

                /* 2SP variant */
                if( CSLCount( papszStdParallels ) == 2 ) {
                    dfStdP1 = CPLAtofM( papszStdParallels[0] );
                    dfStdP2 = CPLAtofM( papszStdParallels[1] );
                    oSRS.SetLCC( dfStdP1, dfStdP2, dfCenterLat, dfCenterLon,
                                 dfFalseEasting, dfFalseNorthing );
                }
                /* 1SP variant (with standard_parallel or center lon) */
                /* See comments in netcdfdataset.h for this projection. */
                else {

                    dfScale = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_SCALE_FACTOR_ORIGIN, -1.0 );

                    /* CF definition, without scale factor */
                    if( CPLIsEqual(dfScale, -1.0) ) {

                        /* with standard_parallel */
                        if( CSLCount( papszStdParallels ) == 1 )
                            dfStdP1 = CPLAtofM( papszStdParallels[0] );
                        /* with center lon instead */
                        else 
                            dfStdP1 = dfCenterLat;
                        /*dfStdP2 = dfStdP1;*/

                        /* test if we should actually compute scale factor */
                        if ( ! CPLIsEqual( dfStdP1, dfCenterLat ) ) {
                            CPLError( CE_Warning, CPLE_NotSupported, 
                                      "NetCDF driver import of LCC-1SP with standard_parallel1 != latitude_of_projection_origin\n"
                                      "(which forces a computation of scale_factor) is experimental (bug #3324)\n" );
                            /* use Snyder eq. 15-4 to compute dfScale from dfStdP1 and dfCenterLat */
                            /* only tested for dfStdP1=dfCenterLat and (25,26), needs more data for testing */
                            /* other option: use the 2SP variant - how to compute new standard parallels? */
                            dfScale = ( cos(dfStdP1) * pow( tan(M_PI/4 + dfStdP1/2), sin(dfStdP1) ) ) /
                                ( cos(dfCenterLat) * pow( tan(M_PI/4 + dfCenterLat/2), sin(dfCenterLat) ) );
                        }
                        /* default is 1.0 */
                        else
                            dfScale = 1.0;

                        oSRS.SetLCC1SP( dfCenterLat, dfCenterLon, dfScale, 
                                        dfFalseEasting, dfFalseNorthing );
                        /* store dfStdP1 so we can output it to CF later */
                        oSRS.SetNormProjParm( SRS_PP_STANDARD_PARALLEL_1, dfStdP1 );
                    }
                    /* OGC/PROJ.4 definition with scale factor */
                    else {
                        oSRS.SetLCC1SP( dfCenterLat, dfCenterLon, dfScale, 
                                        dfFalseEasting, dfFalseNorthing );
                    }
                }

                bGotCfSRS = true;
                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

                CSLDestroy( papszStdParallels );
            }

/* -------------------------------------------------------------------- */
/*      Is this Latitude/Longitude Grid explicitly                      */
/* -------------------------------------------------------------------- */

            else if ( EQUAL ( pszValue, CF_PT_LATITUDE_LONGITUDE ) ) {
                bGotCfSRS = true;
                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );
            }
/* -------------------------------------------------------------------- */
/*      Mercator                                                        */
/* -------------------------------------------------------------------- */

            else if ( EQUAL ( pszValue, CF_PT_MERCATOR ) ) {

                char **papszStdParallels = NULL;

                /* If there is a standard_parallel, know it is Mercator 2SP */
                papszStdParallels = 
                    FetchStandardParallels( szGridMappingValue );

                if (NULL != papszStdParallels) {
                    /* CF-1 Mercator 2SP always has lat centered at equator */
                    dfStdP1 = CPLAtofM( papszStdParallels[0] );

                    dfCenterLat = 0.0;

                    dfCenterLon = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_LON_PROJ_ORIGIN, 0.0 );

                    dfFalseEasting = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_FALSE_EASTING, 0.0 );

                    dfFalseNorthing = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_FALSE_NORTHING, 0.0 );

                    oSRS.SetMercator2SP( dfStdP1, dfCenterLat, dfCenterLon, 
                                      dfFalseEasting, dfFalseNorthing );
                }
                else {
                    dfCenterLon = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_LON_PROJ_ORIGIN, 0.0 );

                    dfCenterLat = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                    dfScale = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_SCALE_FACTOR_ORIGIN,
                                             1.0 );

                    dfFalseEasting = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_FALSE_EASTING, 0.0 );

                    dfFalseNorthing = 
                        poDS->FetchCopyParm( szGridMappingValue, 
                                             CF_PP_FALSE_NORTHING, 0.0 );

                    oSRS.SetMercator( dfCenterLat, dfCenterLon, dfScale, 
                                      dfFalseEasting, dfFalseNorthing );
                }

                bGotCfSRS = true;

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

                CSLDestroy( papszStdParallels );
            }

/* -------------------------------------------------------------------- */
/*      Orthographic                                                    */
/* -------------------------------------------------------------------- */

            else if ( EQUAL ( pszValue, CF_PT_ORTHOGRAPHIC ) ) {
                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LON_PROJ_ORIGIN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;

                oSRS.SetOrthographic( dfCenterLat, dfCenterLon, 
                                      dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );
            }

/* -------------------------------------------------------------------- */
/*      Polar Stereographic                                             */
/* -------------------------------------------------------------------- */

            else if ( EQUAL ( pszValue, CF_PT_POLAR_STEREO ) ) {

                char **papszStdParallels = NULL;

                dfScale = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_SCALE_FACTOR_ORIGIN, 
                                         -1.0 );

                papszStdParallels = 
                    FetchStandardParallels( szGridMappingValue );

                /* CF allows the use of standard_parallel (lat_ts) OR scale_factor (k0),
                   make sure we have standard_parallel, using Snyder eq. 22-7
                   with k=1 and lat=standard_parallel */
                if ( papszStdParallels != NULL ) {
                    dfStdP1 = CPLAtofM( papszStdParallels[0] );
                    /* compute scale_factor from standard_parallel */
                    /* this creates WKT that is inconsistent, don't write for now
                       also proj4 does not seem to use this parameter */
                    // dfScale = ( 1.0 + fabs( sin( dfStdP1 * M_PI / 180.0 ) ) ) / 2.0;
                }
                else {
                    if ( ! CPLIsEqual(dfScale,-1.0) ) {
                        /* compute standard_parallel from scale_factor */
                        dfStdP1 = asin( 2*dfScale - 1 ) * 180.0 / M_PI;

                        /* fetch latitude_of_projection_origin (+90/-90) 
                           used here for the sign of standard_parallel */
                        double dfLatProjOrigin = 
                            poDS->FetchCopyParm( szGridMappingValue, 
                                                 CF_PP_LAT_PROJ_ORIGIN, 
                                                 0.0 );
                        if ( ! CPLIsEqual(dfLatProjOrigin,90.0)  &&
                             ! CPLIsEqual(dfLatProjOrigin,-90.0) ) {
                            CPLError( CE_Failure, CPLE_NotSupported, 
                                      "Polar Stereographic must have a %s parameter equal to +90 or -90\n.",
                                      CF_PP_LAT_PROJ_ORIGIN );
                            dfLatProjOrigin = 90.0;
                        }
                        if ( CPLIsEqual(dfLatProjOrigin,-90.0) )
                            dfStdP1 = - dfStdP1;
                    }
                    else {
                        dfStdP1 = 0.0; //just to avoid warning at compilation
                        CPLError( CE_Failure, CPLE_NotSupported, 
                                  "The NetCDF driver does not support import of CF-1 Polar stereographic "
                                  "without standard_parallel and scale_factor_at_projection_origin parameters.\n" );
                    }
                }

                /* set scale to default value 1.0 if it was not set */
                if ( CPLIsEqual(dfScale,-1.0) )
                    dfScale = 1.0;

                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_VERT_LONG_FROM_POLE, 0.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;
                /* map CF CF_PP_STD_PARALLEL_1 to WKT SRS_PP_LATITUDE_OF_ORIGIN */
                oSRS.SetPS( dfStdP1, dfCenterLon, dfScale, 
                            dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );

                CSLDestroy( papszStdParallels );
            }

/* -------------------------------------------------------------------- */
/*      Stereographic                                                   */
/* -------------------------------------------------------------------- */

            else if ( EQUAL ( pszValue, CF_PT_STEREO ) ) {

                dfCenterLon = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LON_PROJ_ORIGIN, 0.0 );

                dfCenterLat = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_LAT_PROJ_ORIGIN, 0.0 );

                dfScale = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_SCALE_FACTOR_ORIGIN,
                                         1.0 );

                dfFalseEasting = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_EASTING, 0.0 );

                dfFalseNorthing = 
                    poDS->FetchCopyParm( szGridMappingValue, 
                                         CF_PP_FALSE_NORTHING, 0.0 );

                bGotCfSRS = true;
                oSRS.SetStereographic( dfCenterLat, dfCenterLon, dfScale, 
                                       dfFalseEasting, dfFalseNorthing );

                if( !bGotGeogCS )
                    oSRS.SetWellKnownGeogCS( "WGS84" );
            }

/* -------------------------------------------------------------------- */
/*      Is this Latitude/Longitude Grid, default                        */
/* -------------------------------------------------------------------- */

        } else if( EQUAL( szDimNameX, NCDF_DIMNAME_LON ) ) {
            oSRS.SetWellKnownGeogCS( "WGS84" );

        } else {
            // This would be too indiscriminate.  But we should set
            // it if we know the data is geographic.
            // oSRS.SetWellKnownGeogCS( "WGS84" );
        }
    }
/* -------------------------------------------------------------------- */
/*      Read projection coordinates                                     */
/* -------------------------------------------------------------------- */

    int nVarDimXID = -1;
    int nVarDimYID = -1;
    if( !bReadSRSOnly )
    {
        nc_inq_varid( cdfid, poDS->papszDimName[nXDimID], &nVarDimXID );
        nc_inq_varid( cdfid, poDS->papszDimName[nYDimID], &nVarDimYID );
    }

    size_t start[2], edge[2];
    if( !bReadSRSOnly && ( nVarDimXID != -1 ) && ( nVarDimYID != -1 ) ) {
        pdfXCoord = (double *) CPLCalloc( xdim, sizeof(double) );
        pdfYCoord = (double *) CPLCalloc( ydim, sizeof(double) );

        start[0] = 0;
        edge[0]  = xdim;
        int status = nc_get_vara_double( cdfid, nVarDimXID, 
                                     start, edge, pdfXCoord);
        NCDF_ERR(status);

        edge[0]  = ydim;
        status = nc_get_vara_double( cdfid, nVarDimYID, 
                                     start, edge, pdfYCoord);
        NCDF_ERR(status);

/* -------------------------------------------------------------------- */
/*      Check for bottom-up from the Y-axis order                       */
/*      see bugs #4284 and #4251                                        */
/* -------------------------------------------------------------------- */

        poDS->bBottomUp = (pdfYCoord[0] <= pdfYCoord[1]);

        CPLDebug( "GDAL_netCDF", "set bBottomUp = %d from Y axis",
                  static_cast<int>(poDS->bBottomUp) );

/* -------------------------------------------------------------------- */
/*      convert ]180,360] longitude values to [-180,180]                */
/* -------------------------------------------------------------------- */

        if ( NCDFIsVarLongitude( cdfid, nVarDimXID, NULL ) &&
             CPLTestBool(CPLGetConfigOption("GDAL_NETCDF_CENTERLONG_180", "YES"))) {
            // If minimum longitude is > 180, subtract 360 from all.
            if ( MIN( pdfXCoord[0], pdfXCoord[xdim-1] ) > 180.0 ) {
                for ( size_t i=0; i<xdim ; i++ )
                        pdfXCoord[i] -= 360;
            }
        }

/* -------------------------------------------------------------------- */
/*     Set Projection from CF                                           */
/* -------------------------------------------------------------------- */
    if ( bGotGeogCS || bGotCfSRS ) {
        /* Set SRS Units */

        /* check units for x and y */
        if( oSRS.IsProjected( ) ) {
            snprintf(szTemp,sizeof(szTemp), "%s#units", poDS->papszDimName[nXDimID]);
            const char *pszUnitsX = CSLFetchNameValue( poDS->papszMetadata, 
                                          szTemp );

            snprintf(szTemp,sizeof(szTemp), "%s#units", poDS->papszDimName[nYDimID]);
            const char *pszUnitsY = CSLFetchNameValue( poDS->papszMetadata, 
                                          szTemp );

            /* TODO: what to do if units are not equal in X and Y */
            if ( (pszUnitsX != NULL) && (pszUnitsY != NULL) && 
                 EQUAL(pszUnitsX,pszUnitsY) )
                pszUnits = pszUnitsX;

            /* add units to PROJCS */
            if ( pszUnits != NULL && ! EQUAL(pszUnits,"") ) {
                CPLDebug( "GDAL_netCDF", 
                          "units=%s", pszUnits );
                if ( EQUAL(pszUnits,"m") ) {
                    oSRS.SetLinearUnits( "metre", 1.0 );
                    oSRS.SetAuthority( "PROJCS|UNIT", "EPSG", 9001 );
                }
                else if ( EQUAL(pszUnits,"km") ) {
                    oSRS.SetLinearUnits( "kilometre", 1000.0 );
                    oSRS.SetAuthority( "PROJCS|UNIT", "EPSG", 9036 );
                }
                /* TODO check for other values */
                // else
                //     oSRS.SetLinearUnits(pszUnits, 1.0);
            }
        }
        else if ( oSRS.IsGeographic() ) {
            oSRS.SetAngularUnits( CF_UNITS_D, CPLAtof(SRS_UA_DEGREE_CONV) );
            oSRS.SetAuthority( "GEOGCS|UNIT", "EPSG", 9122 );
        }

        /* Set Projection */
        oSRS.exportToWkt( &(pszTempProjection) );
        CPLDebug( "GDAL_netCDF", "setting WKT from CF" );
        SetProjection( pszTempProjection );
        CPLFree( pszTempProjection );

        if ( !bGotCfGT )
            CPLDebug( "GDAL_netCDF", "got SRS but no geotransform from CF!");
    }

/* -------------------------------------------------------------------- */
/*      Is pixel spacing uniform across the map?                       */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/*      Check Longitude                                                 */
/* -------------------------------------------------------------------- */

        if( xdim == 2 ) {
            bLonSpacingOK = true;
        }
        else
        {
            nSpacingBegin   = (int) poDS->rint((pdfXCoord[1] - pdfXCoord[0]) * 1000);

            nSpacingMiddle  = (int) poDS->rint((pdfXCoord[xdim/2+1] - 
                                                pdfXCoord[xdim/2]) * 1000);

            nSpacingLast    = (int) poDS->rint((pdfXCoord[xdim-1] - 
                                                pdfXCoord[xdim-2]) * 1000);       

            CPLDebug("GDAL_netCDF", 
                     "xdim: %ld nSpacingBegin: %d nSpacingMiddle: %d nSpacingLast: %d",
                     (long)xdim, nSpacingBegin, nSpacingMiddle, nSpacingLast );
#ifdef NCDF_DEBUG
            CPLDebug("GDAL_netCDF", 
                     "xcoords: %f %f %f %f %f %f",
                     pdfXCoord[0], pdfXCoord[1], pdfXCoord[xdim / 2], pdfXCoord[(xdim / 2) + 1],
                     pdfXCoord[xdim - 2], pdfXCoord[xdim-1]);
#endif

            if( ( abs( abs( nSpacingBegin ) - abs( nSpacingLast ) )  <= 1   ) &&
                ( abs( abs( nSpacingBegin ) - abs( nSpacingMiddle ) ) <= 1 ) &&
                ( abs( abs( nSpacingMiddle ) - abs( nSpacingLast ) ) <= 1   ) ) {
                bLonSpacingOK = true;
            }
        }

        if ( bLonSpacingOK == false ) {
            CPLDebug( "GDAL_netCDF", 
                      "Longitude is not equally spaced." );
        }

/* -------------------------------------------------------------------- */
/*      Check Latitude                                                  */
/* -------------------------------------------------------------------- */
        if( ydim == 2 ) {
            bLatSpacingOK = true;
        }
        else
        {
            nSpacingBegin   = (int) poDS->rint((pdfYCoord[1] - pdfYCoord[0]) * 
                                               1000); 	    

            nSpacingMiddle  = (int) poDS->rint((pdfYCoord[ydim/2+1] - 
                                                pdfYCoord[ydim/2]) * 
                                               1000);

            nSpacingLast    = (int) poDS->rint((pdfYCoord[ydim-1] - 
                                                pdfYCoord[ydim-2]) * 
                                               1000);

            CPLDebug("GDAL_netCDF", 
                     "ydim: %ld nSpacingBegin: %d nSpacingMiddle: %d nSpacingLast: %d",
                     (long)ydim, nSpacingBegin, nSpacingMiddle, nSpacingLast );
#ifdef NCDF_DEBUG
            CPLDebug("GDAL_netCDF", 
                     "ycoords: %f %f %f %f %f %f",
                     pdfYCoord[0], pdfYCoord[1], pdfYCoord[ydim / 2], pdfYCoord[(ydim / 2) + 1],
                     pdfYCoord[ydim - 2], pdfYCoord[ydim-1]);
#endif

/* -------------------------------------------------------------------- */
/*   For Latitude we allow an error of 0.1 degrees for gaussian         */
/*   gridding (only if this is not a projected SRS)                     */
/* -------------------------------------------------------------------- */

            if( ( abs( abs( nSpacingBegin )  - abs( nSpacingLast ) )  <= 1   ) &&
                ( abs( abs( nSpacingBegin )  - abs( nSpacingMiddle ) ) <= 1 ) &&
                ( abs( abs( nSpacingMiddle ) - abs( nSpacingLast ) ) <= 1   ) ) {
                bLatSpacingOK = true;
            }
            else if( !oSRS.IsProjected() &&
                     ( (( abs( abs(nSpacingBegin)  - abs(nSpacingLast) ) )   <= 100 ) &&
                       (( abs( abs(nSpacingBegin)  - abs(nSpacingMiddle) ) ) <= 100 ) &&
                       (( abs( abs(nSpacingMiddle) - abs(nSpacingLast) ) )   <= 100 ) ) ) {
                bLatSpacingOK = true;
                CPLError(CE_Warning, 1,"Latitude grid not spaced evenly.\nSetting projection for grid spacing is within 0.1 degrees threshold.\n");

                CPLDebug("GDAL_netCDF", 
                         "Latitude grid not spaced evenly, but within 0.1 degree threshold (probably a Gaussian grid).\n"
                         "Saving original latitude values in Y_VALUES geolocation metadata" );
                Set1DGeolocation( nVarDimYID, "Y" );
            }

            if ( bLatSpacingOK == false ) {
                CPLDebug( "GDAL_netCDF", 
                          "Latitude is not equally spaced." );
            }
        }
        if ( ( bLonSpacingOK ) && ( bLatSpacingOK ) ) {

/* -------------------------------------------------------------------- */
/*      We have gridded data so we can set the Gereferencing info.      */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/*      Enable GeoTransform                                             */
/* -------------------------------------------------------------------- */
                /* ----------------------------------------------------------*/
                /*    In the following "actual_range" and "node_offset"      */
                /*    are attributes used by netCDF files created by GMT.    */
                /*    If we find them we know how to proceed. Else, use      */
                /*    the original algorithm.                                */
                /* --------------------------------------------------------- */
                bGotCfGT = true;

                int node_offset = 0;
                nc_get_att_int (cdfid, NC_GLOBAL, "node_offset", &node_offset);

                double	dummy[2], xMinMax[2], yMinMax[2];

                if (!nc_get_att_double (cdfid, nVarDimXID, "actual_range", dummy)) {
                    xMinMax[0] = dummy[0];
                    xMinMax[1] = dummy[1];
                }
                else {
                    xMinMax[0] = pdfXCoord[0];
                    xMinMax[1] = pdfXCoord[xdim-1];
                    node_offset = 0;
                }

                if (!nc_get_att_double (cdfid, nVarDimYID, "actual_range", dummy)) {
                    yMinMax[0] = dummy[0];
                    yMinMax[1] = dummy[1];
                }
                else {
                    yMinMax[0] = pdfYCoord[0];	
                    yMinMax[1] = pdfYCoord[ydim-1];
                    node_offset = 0;
                }

                /* Check for reverse order of y-coordinate */
                if ( yMinMax[0] > yMinMax[1] ) {
                    dummy[0] = yMinMax[1];
                    dummy[1] = yMinMax[0];
                    yMinMax[0] = dummy[0];
                    yMinMax[1] = dummy[1];
                }

                adfTempGeoTransform[0] = xMinMax[0];
                adfTempGeoTransform[2] = 0;
                adfTempGeoTransform[3] = yMinMax[1];
                adfTempGeoTransform[4] = 0;
                adfTempGeoTransform[1] = ( xMinMax[1] - xMinMax[0] ) / 
                    ( poDS->nRasterXSize + (node_offset - 1) );
                adfTempGeoTransform[5] = ( yMinMax[0] - yMinMax[1] ) / 
                    ( poDS->nRasterYSize + (node_offset - 1) );

/* -------------------------------------------------------------------- */
/*     Compute the center of the pixel                                  */
/* -------------------------------------------------------------------- */
                if ( !node_offset ) {	// Otherwise its already the pixel center
                    adfTempGeoTransform[0] -= (adfTempGeoTransform[1] / 2);
                    adfTempGeoTransform[3] -= (adfTempGeoTransform[5] / 2);
                }

        }

        CPLFree( pdfXCoord );
        CPLFree( pdfYCoord );
    }// end if (has dims)

/* -------------------------------------------------------------------- */
/*      Process custom GDAL values (spatial_ref, GeoTransform)          */
/* -------------------------------------------------------------------- */
    if( !EQUAL( szGridMappingValue, "" )  ) {

        if( pszWKT != NULL ) {

/* -------------------------------------------------------------------- */
/*      Compare SRS obtained from CF attributes and GDAL WKT            */
/*      If possible use the more complete GDAL WKT                      */
/* -------------------------------------------------------------------- */
            /* Set the SRS to the one written by GDAL */
            if ( ! bGotCfSRS || poDS->pszProjection == NULL || ! bIsGdalCfFile ) {   
                bGotGdalSRS = true;
                CPLDebug( "GDAL_netCDF", "setting WKT from GDAL" );
                SetProjection( pszWKT );
            }
            else { /* use the SRS from GDAL if it doesn't conflict with the one from CF */
                char *pszProjectionGDAL = (char*) pszWKT ;
                OGRSpatialReference oSRSGDAL;
                oSRSGDAL.importFromWkt( &pszProjectionGDAL );
                /* set datum to unknown or else datums will not match, see bug #4281 */
                if ( oSRSGDAL.GetAttrNode( "DATUM" ) )
                    oSRSGDAL.GetAttrNode( "DATUM" )->GetChild(0)->SetValue( "unknown" );
                /* need this for setprojection autotest */ 
                if ( oSRSGDAL.GetAttrNode( "PROJCS" ) )
                    oSRSGDAL.GetAttrNode( "PROJCS" )->GetChild(0)->SetValue( "unnamed" );
                if ( oSRSGDAL.GetAttrNode( "GEOGCS" ) )
                    oSRSGDAL.GetAttrNode( "GEOGCS" )->GetChild(0)->SetValue( "unknown" );   
                oSRSGDAL.GetRoot()->StripNodes( "UNIT" );
                if ( oSRS.IsSame(&oSRSGDAL) ) {
                    // printf("ARE SAME, using GDAL WKT\n");
                    bGotGdalSRS = true;
                    CPLDebug( "GDAL_netCDF", "setting WKT from GDAL" );
                    SetProjection( pszWKT );
                }
                else {
                    CPLDebug( "GDAL_netCDF", 
                              "got WKT from GDAL \n[%s]\nbut not using it because conflicts with CF\n[%s]\n", 
                              pszWKT, poDS->pszProjection );
                }
            }

/* -------------------------------------------------------------------- */
/*      Look for GeoTransform Array, if not found in CF                 */
/* -------------------------------------------------------------------- */
            if ( !bGotCfGT ) {

                /* TODO read the GT values and detect for conflict with CF */
                /* this could resolve the GT precision loss issue  */

                if( pszGeoTransform != NULL ) {

                    char** papszGeoTransform = CSLTokenizeString2( pszGeoTransform,
                                                            " ", 
                                                            CSLT_HONOURSTRINGS );
                    if( CSLCount(papszGeoTransform) == 6 )
                    {
                        bGotGdalGT = true;
                        for(int i=0;i<6;i++)
                            adfTempGeoTransform[i] = CPLAtof( papszGeoTransform[i] );
                    }
                    CSLDestroy( papszGeoTransform );
/* -------------------------------------------------------------------- */
/*      Look for corner array values                                    */
/* -------------------------------------------------------------------- */
                } else {
                    // CPLDebug( "GDAL_netCDF", "looking for geotransform corners\n" );

                    snprintf(szTemp,sizeof(szTemp), "%s#Northernmost_Northing", szGridMappingValue);
                    pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);
                    bool bGotNN = false;
                    double dfNN = 0.0;
                    if( pszValue != NULL ) {
                        dfNN = CPLAtof( pszValue );
                        bGotNN = true;
                    }

                    snprintf(szTemp,sizeof(szTemp), "%s#Southernmost_Northing", szGridMappingValue);
                    pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);
                    bool bGotSN = false;
                    double dfSN = 0.0;
                    if( pszValue != NULL ) {
                        dfSN = CPLAtof( pszValue );
                        bGotSN = true;
                    }

                    snprintf(szTemp,sizeof(szTemp), "%s#Easternmost_Easting", szGridMappingValue);
                    pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);
                    bool bGotEE = false;
                    double dfEE = 0.0;
                    if( pszValue != NULL ) {
                        dfEE = CPLAtof( pszValue );
                        bGotEE = true;
                    }

                    snprintf(szTemp,sizeof(szTemp), "%s#Westernmost_Easting", szGridMappingValue);
                    pszValue = CSLFetchNameValue(poDS->papszMetadata, szTemp);
                    bool bGotWE = false;
                    double dfWE = 0.0;
                    if( pszValue != NULL ) {
                        dfWE = CPLAtof( pszValue );
                        bGotWE = true;
                    }

                    /* Only set the GeoTransform if we got all the values */
                    if ( bGotNN && bGotSN && bGotEE && bGotWE ) {

                        bGotGdalGT = true;

                        adfTempGeoTransform[0] = dfWE;
                        adfTempGeoTransform[1] = (dfEE - dfWE) /
                            ( poDS->GetRasterXSize() - 1 );
                        adfTempGeoTransform[2] = 0.0;
                        adfTempGeoTransform[3] = dfNN;
                        adfTempGeoTransform[4] = 0.0;
                        adfTempGeoTransform[5] = (dfSN - dfNN) /
                            ( poDS->GetRasterYSize() - 1 );
                        /* compute the center of the pixel */
                        adfTempGeoTransform[0] = dfWE
                            - (adfTempGeoTransform[1] / 2);
                        adfTempGeoTransform[3] = dfNN
                            - (adfTempGeoTransform[5] / 2);
                    }
                } // (pszGeoTransform != NULL)

                if ( bGotGdalSRS && ! bGotGdalGT )
                    CPLDebug( "GDAL_netCDF",
                              "Got SRS but no geotransform from GDAL!");

            } // if ( !bGotCfGT )

        }
    }

    // Set GeoTransform if we got a complete one - after projection has been set
    if ( bGotCfGT || bGotGdalGT ) {
        SetGeoTransform( adfTempGeoTransform );
    }

    // Process geolocation arrays from CF "coordinates" attribute.
    // Perhaps we should only add if is not a (supported) CF projection
    // (bIsCfProjection).
    ProcessCFGeolocation( nVarId );

    // Debugging reports.
    CPLDebug( "GDAL_netCDF",
              "bGotGeogCS=%d bGotCfSRS=%d bGotCfGT=%d bGotGdalSRS=%d "
              "bGotGdalGT=%d",
              static_cast<int>(bGotGeogCS), static_cast<int>(bGotCfSRS),
              static_cast<int>(bGotCfGT), static_cast<int>(bGotGdalSRS),
              static_cast<int>(bGotGdalGT) );

    if ( !bGotCfGT && !bGotGdalGT )
        CPLDebug( "GDAL_netCDF", "did not get geotransform from CF nor GDAL!");

    if ( !bGotGeogCS && !bGotCfSRS && !bGotGdalSRS && !bGotCfGT)
        CPLDebug( "GDAL_netCDF",  "did not get projection from CF nor GDAL!");

/* -------------------------------------------------------------------- */
/*     Search for Well-known GeogCS if got only CF WKT                  */
/*     Disabled for now, as a named datum also include control points   */
/*     (see mailing list and bug#4281                                   */
/*     For example, WGS84 vs. GDA94 (EPSG:3577) - AEA in netcdf_cf.py   */
/* -------------------------------------------------------------------- */
    /* disabled for now, but could be set in a config option */
#if 0
    bool bLookForWellKnownGCS = false;  // This could be a Config Option.

    if ( bLookForWellKnownGCS && bGotCfSRS && ! bGotGdalSRS ) {
        /* ET - could use a more exhaustive method by scanning all EPSG codes in data/gcs.csv */
        /* as proposed by Even in the gdal-dev mailing list "help for comparing two WKT" */
        /* this code could be contributed to a new function */
        /* OGRSpatialReference * OGRSpatialReference::FindMatchingGeogCS( const OGRSpatialReference *poOther ) */
        CPLDebug( "GDAL_netCDF", "Searching for Well-known GeogCS" );
        const char *pszWKGCSList[] = { "WGS84", "WGS72", "NAD27", "NAD83" };
        char *pszWKGCS = NULL;
        oSRS.exportToPrettyWkt( &pszWKGCS );
        for( size_t i=0; i<sizeof(pszWKGCSList)/8; i++ ) {
            pszWKGCS = CPLStrdup( pszWKGCSList[i] );
            OGRSpatialReference oSRSTmp;
            oSRSTmp.SetWellKnownGeogCS( pszWKGCSList[i] );
            /* set datum to unknown, bug #4281 */
            if ( oSRSTmp.GetAttrNode( "DATUM" ) )
                oSRSTmp.GetAttrNode( "DATUM" )->GetChild(0)->SetValue( "unknown" );
            /* could use  OGRSpatialReference::StripCTParms() but let's keep TOWGS84 */
            oSRSTmp.GetRoot()->StripNodes( "AXIS" );
            oSRSTmp.GetRoot()->StripNodes( "AUTHORITY" );
            oSRSTmp.GetRoot()->StripNodes( "EXTENSION" );

            oSRSTmp.exportToPrettyWkt( &pszWKGCS );
            if ( oSRS.IsSameGeogCS(&oSRSTmp) ) {
                oSRS.SetWellKnownGeogCS( pszWKGCSList[i] );
                oSRS.exportToWkt( &(pszTempProjection) );
                SetProjection( pszTempProjection );
                CPLFree( pszTempProjection );
            }
        }
    }
#endif
}


int netCDFDataset::ProcessCFGeolocation( int nVarId )
{
    bool bAddGeoloc = false;
    char *pszTemp = NULL;

    if ( NCDFGetAttr( cdfid, nVarId, "coordinates", &pszTemp ) == CE_None ) { 
        /* get X and Y geolocation names from coordinates attribute */
        char **papszTokens = CSLTokenizeString2( pszTemp, " ", 0 );
        if ( CSLCount(papszTokens) >= 2 ) {

            char szGeolocXName[NC_MAX_NAME+1];
            char szGeolocYName[NC_MAX_NAME+1];
            szGeolocXName[0] = '\0';
            szGeolocYName[0] = '\0';

            /* test that each variable is longitude/latitude */
            for ( int i=0; i<CSLCount(papszTokens); i++ ) {
                if ( NCDFIsVarLongitude(cdfid, -1, papszTokens[i]) ) 
                    snprintf(szGeolocXName,sizeof(szGeolocXName),"%s",papszTokens[i] );
                else if ( NCDFIsVarLatitude(cdfid, -1, papszTokens[i]) ) 
                    snprintf(szGeolocYName,sizeof(szGeolocYName),"%s",papszTokens[i] );
            }
            /* add GEOLOCATION metadata */
            if ( !EQUAL(szGeolocXName,"") && !EQUAL(szGeolocYName,"") ) {
                bAddGeoloc = true;
                CPLDebug( "GDAL_netCDF", 
                          "using variables %s and %s for GEOLOCATION",
                          szGeolocXName, szGeolocYName );

                SetMetadataItem( "SRS", SRS_WKT_WGS84, "GEOLOCATION" );

                CPLString osTMP;
                osTMP.Printf( "NETCDF:\"%s\":%s",
                              osFilename.c_str(), szGeolocXName );
                SetMetadataItem( "X_DATASET", osTMP, "GEOLOCATION" );
                SetMetadataItem( "X_BAND", "1" , "GEOLOCATION" );
                osTMP.Printf( "NETCDF:\"%s\":%s",
                              osFilename.c_str(), szGeolocYName );
                SetMetadataItem( "Y_DATASET", osTMP, "GEOLOCATION" );
                SetMetadataItem( "Y_BAND", "1" , "GEOLOCATION" );

                SetMetadataItem( "PIXEL_OFFSET", "0", "GEOLOCATION" );
                SetMetadataItem( "PIXEL_STEP", "1", "GEOLOCATION" );

                SetMetadataItem( "LINE_OFFSET", "0", "GEOLOCATION" );
                SetMetadataItem( "LINE_STEP", "1", "GEOLOCATION" );
            }
            else {
                CPLDebug( "GDAL_netCDF", 
                          "coordinates attribute [%s] is unsupported",
                          pszTemp );
            }
        }
        else {
            CPLDebug( "GDAL_netCDF", 
                      "coordinates attribute [%s] with %d element(s) is unsupported",
                      pszTemp, CSLCount(papszTokens) );
        }
        if (papszTokens) CSLDestroy(papszTokens);
        CPLFree( pszTemp );
    }

    return bAddGeoloc;
}

CPLErr netCDFDataset::Set1DGeolocation( int nVarId, const char *szDimName )
{
    /* get values */
    char    *pszVarValues = NULL;
    CPLErr eErr = NCDFGet1DVar( cdfid, nVarId, &pszVarValues );
    if ( eErr != CE_None )
        return eErr;

    /* write metadata */
    char szTemp[ NC_MAX_NAME + 1 + 32 ];
    snprintf( szTemp, sizeof(szTemp), "%s_VALUES", szDimName );
    SetMetadataItem( szTemp, pszVarValues, "GEOLOCATION2" );

    CPLFree( pszVarValues );

    return CE_None;
}


double *netCDFDataset::Get1DGeolocation( CPL_UNUSED const char *szDimName, int &nVarLen )
{
    nVarLen = 0;

    /* get Y_VALUES as tokens */
    char **papszValues
        = NCDFTokenizeArray( GetMetadataItem( "Y_VALUES", "GEOLOCATION2" ) );
    if ( papszValues == NULL )
        return NULL;

    /* initialize and fill array */
    nVarLen = CSLCount(papszValues);
    double *pdfVarValues = (double *) CPLCalloc( nVarLen, sizeof( double ) );

    for(int i=0, j=0; i < nVarLen; i++) { 
        if ( ! bBottomUp ) j=nVarLen - 1 - i;
        else j=i; /* invert latitude values */
        char *pszTemp = NULL;
        pdfVarValues[j] = CPLStrtod( papszValues[i], &pszTemp );
    }
    CSLDestroy( papszValues );

    return pdfVarValues;
}


/************************************************************************/
/*                          SetProjection()                           */
/************************************************************************/
CPLErr 	netCDFDataset::SetProjection( const char * pszNewProjection )
{
    CPLMutexHolderD(&hNCMutex);

/* TODO look if proj. already defined, like in geotiff */
    if( pszNewProjection == NULL ) 
    {
        CPLError( CE_Failure, CPLE_AppDefined, "NULL projection." );
        return CE_Failure;
    }

    if( bSetProjection && (GetAccess() == GA_Update) ) 
    {
        CPLError( CE_Warning, CPLE_AppDefined,
                  "netCDFDataset::SetProjection() should only be called once "
                  "in update mode!\npszNewProjection=\n%s",
                  pszNewProjection );
    }

    CPLDebug( "GDAL_netCDF", "SetProjection, WKT = %s", pszNewProjection );

    if( !STARTS_WITH_CI(pszNewProjection, "GEOGCS")
        && !STARTS_WITH_CI(pszNewProjection, "PROJCS")
        && !EQUAL(pszNewProjection,"") )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Only OGC WKT GEOGCS and PROJCS Projections supported for writing to NetCDF.\n"
                  "%s not supported.",
                  pszNewProjection );

        return CE_Failure;
    }

    CPLFree( pszProjection );
    pszProjection = CPLStrdup( pszNewProjection );

    if( GetAccess() == GA_Update )
    {
        if ( bSetGeoTransform && ! bSetProjection ) {
            bSetProjection = true;
            return AddProjectionVars();
        }
    }

    bSetProjection = true;

    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr 	netCDFDataset::SetGeoTransform ( double * padfTransform )
{
    CPLMutexHolderD(&hNCMutex);

    memcpy( adfGeoTransform, padfTransform, sizeof(double)*6 );
    // bGeoTransformValid = TRUE;
    // bGeoTIFFInfoChanged = TRUE;

    CPLDebug( "GDAL_netCDF", 
              "SetGeoTransform(%f,%f,%f,%f,%f,%f)",
              padfTransform[0],padfTransform[1],padfTransform[2],
              padfTransform[3],padfTransform[4],padfTransform[5]);

    if( GetAccess() == GA_Update )
    {
        if ( bSetProjection && ! bSetGeoTransform ) {
            bSetGeoTransform = true;
            return AddProjectionVars();
        }
    }

    bSetGeoTransform = true;

    return CE_None;
}


/************************************************************************/
/*                         NCDFWriteSRSVariable()                       */
/************************************************************************/

static int NCDFWriteSRSVariable(int cdfid, OGRSpatialReference* poSRS,
                                char** ppszCFProjection, bool bWriteGDALTags)
{
    int status;
    int NCDFVarID = -1;
    char* pszCFProjection = NULL;

    *ppszCFProjection = NULL;

    if( poSRS->IsProjected() )
    {
/* -------------------------------------------------------------------- */
/*      Write CF-1.5 compliant Projected attributes                     */
/* -------------------------------------------------------------------- */

        const OGR_SRSNode *poPROJCS = poSRS->GetAttrNode( "PROJCS" );
        const char  *pszProjName;
        pszProjName = poSRS->GetAttrValue( "PROJECTION" );
        if( pszProjName == NULL )
            return -1;

        /* Basic Projection info (grid_mapping and datum) */
        for( int i=0; poNetcdfSRS_PT[i].WKT_SRS != NULL; i++ ) {
            if( EQUAL( poNetcdfSRS_PT[i].WKT_SRS, pszProjName ) ) {
                CPLDebug( "GDAL_netCDF", "GDAL PROJECTION = %s , NCDF PROJECTION = %s", 
                            poNetcdfSRS_PT[i].WKT_SRS, 
                            poNetcdfSRS_PT[i].CF_SRS);
                pszCFProjection = CPLStrdup( poNetcdfSRS_PT[i].CF_SRS );
                CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d)",
                            cdfid, poNetcdfSRS_PT[i].CF_SRS, NC_CHAR ); 
                status = nc_def_var( cdfid, 
                                        poNetcdfSRS_PT[i].CF_SRS,
                                        NC_CHAR, 
                                        0, NULL, &NCDFVarID );
                NCDF_ERR(status);
                break;
            }
        }
        if( pszCFProjection == NULL )
            return -1;

        status = nc_put_att_text( cdfid, NCDFVarID, CF_GRD_MAPPING_NAME,
                                    strlen( pszCFProjection ),
                                    pszCFProjection );
        NCDF_ERR(status);

        /* Various projection attributes */
        // PDS: keep in sync with SetProjection function
        NCDFWriteProjAttribs(poPROJCS, pszProjName, cdfid, NCDFVarID);
    }
    else 
    {
/* -------------------------------------------------------------------- */
/*      Write CF-1.5 compliant Geographics attributes                   */
/*      Note: WKT information will not be preserved (e.g. WGS84)        */
/* -------------------------------------------------------------------- */

        pszCFProjection = CPLStrdup( "crs" );
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d)",
                    cdfid, pszCFProjection, NC_CHAR );
        status = nc_def_var( cdfid, pszCFProjection, NC_CHAR, 
                                0, NULL, &NCDFVarID );
        NCDF_ERR(status);
        status = nc_put_att_text( cdfid, NCDFVarID, CF_GRD_MAPPING_NAME,
                                    strlen(CF_PT_LATITUDE_LONGITUDE),
                                    CF_PT_LATITUDE_LONGITUDE );
        NCDF_ERR(status);
    }

    status = nc_put_att_text( cdfid, NCDFVarID, CF_LNG_NAME,
                              strlen("CRS definition"),
                             "CRS definition" );
    NCDF_ERR(status);

    *ppszCFProjection = pszCFProjection;

/* -------------------------------------------------------------------- */
/*      Write CF-1.5 compliant common attributes                        */
/* -------------------------------------------------------------------- */

    /* DATUM information */
    double dfTemp = poSRS->GetPrimeMeridian();
    nc_put_att_double( cdfid, NCDFVarID, CF_PP_LONG_PRIME_MERIDIAN,
                        NC_DOUBLE, 1, &dfTemp );
    dfTemp = poSRS->GetSemiMajor();
    nc_put_att_double( cdfid, NCDFVarID, CF_PP_SEMI_MAJOR_AXIS,
                        NC_DOUBLE, 1, &dfTemp );
    dfTemp = poSRS->GetInvFlattening();
    nc_put_att_double( cdfid, NCDFVarID, CF_PP_INVERSE_FLATTENING,
                        NC_DOUBLE, 1, &dfTemp );

    if( bWriteGDALTags )
    {
        char* pszSpatialRef = NULL;
        poSRS->exportToWkt(&pszSpatialRef);
        status = nc_put_att_text( cdfid, NCDFVarID, NCDF_SPATIAL_REF,
                                  strlen( pszSpatialRef ), pszSpatialRef );
        NCDF_ERR(status);
        CPLFree(pszSpatialRef);
    }

    return NCDFVarID;
}

/************************************************************************/
/*                   NCDFWriteLonLatVarsAttributes()                    */
/************************************************************************/

static void NCDFWriteLonLatVarsAttributes(int cdfid,
                                          int nVarLonID,
                                          int nVarLatID)
{
    int status;

    status = nc_put_att_text( cdfid, nVarLatID, CF_STD_NAME,
                     strlen(CF_LATITUDE_STD_NAME), CF_LATITUDE_STD_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarLatID, CF_LNG_NAME,
                     strlen(CF_LATITUDE_LNG_NAME), CF_LATITUDE_LNG_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarLatID, CF_UNITS,
                     strlen(CF_DEGREES_NORTH), CF_DEGREES_NORTH );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarLonID, CF_STD_NAME,
                     strlen(CF_LONGITUDE_STD_NAME), CF_LONGITUDE_STD_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarLonID, CF_LNG_NAME,
                     strlen(CF_LONGITUDE_LNG_NAME), CF_LONGITUDE_LNG_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarLonID, CF_UNITS,
                     strlen(CF_DEGREES_EAST), CF_DEGREES_EAST );
    NCDF_ERR(status);
}

/************************************************************************/
/*                     NCDFWriteXYVarsAttributes()                      */
/************************************************************************/

static void NCDFWriteXYVarsAttributes(int cdfid, int nVarXID, int nVarYID,
                                      OGRSpatialReference* poSRS)
{
    int status;
    const char *pszUnits = NULL;
    const char *pszUnitsToWrite = "";

    pszUnits = poSRS->GetAttrValue("PROJCS|UNIT",1);
    if ( pszUnits == NULL || EQUAL(pszUnits,"1") ) 
        pszUnitsToWrite = "m";
    else if ( EQUAL(pszUnits,"1000") ) 
        pszUnitsToWrite = "km";

    status = nc_put_att_text( cdfid, nVarXID, CF_STD_NAME,
                        strlen(CF_PROJ_X_COORD),
                        CF_PROJ_X_COORD );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarXID, CF_LNG_NAME,
                        strlen(CF_PROJ_X_COORD_LONG_NAME),
                        CF_PROJ_X_COORD_LONG_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarXID, CF_UNITS,
                        strlen(pszUnitsToWrite), pszUnitsToWrite ); 
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarYID, CF_STD_NAME,
                        strlen(CF_PROJ_Y_COORD),
                        CF_PROJ_Y_COORD );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarYID, CF_LNG_NAME,
                        strlen(CF_PROJ_Y_COORD_LONG_NAME),
                        CF_PROJ_Y_COORD_LONG_NAME );
    NCDF_ERR(status);

    status = nc_put_att_text( cdfid, nVarYID, CF_UNITS,
                        strlen(pszUnitsToWrite), pszUnitsToWrite ); 
    NCDF_ERR(status);
}

/************************************************************************/
/*                          AddProjectionVars()                         */
/************************************************************************/

CPLErr netCDFDataset::AddProjectionVars( GDALProgressFunc pfnProgress, 
                                         void * pProgressData )
{
    int NCDFVarID = -1;
    const char  *pszValue = NULL;
    CPLErr eErr = CE_None;

    bool bWriteGridMapping = false;
    bool bWriteLonLat = false;
    bool bHasGeoloc = false;
    bool bWriteGDALTags = false;
    bool bWriteGeoTransform = false;

    nc_type eLonLatType = NC_NAT;
    int nVarLonID=-1, nVarLatID=-1;
    int nVarXID=-1, nVarYID=-1;

    /* For GEOLOCATION information */
    const char *pszDSName = NULL;
    GDALDatasetH     hDS_X = NULL;
    GDALRasterBandH  hBand_X = NULL;
    GDALDatasetH     hDS_Y = NULL;
    GDALRasterBandH  hBand_Y = NULL;
    int nBand;

    bAddedProjectionVars = true;

    char *pszWKT = (char *) pszProjection;
    OGRSpatialReference oSRS;
    oSRS.importFromWkt( &pszWKT );

    if( oSRS.IsProjected() )
        bIsProjected = true;
    else if( oSRS.IsGeographic() )
        bIsGeographic = true;

    CPLDebug( "GDAL_netCDF", "SetProjection, WKT now = [%s]\nprojected: %d geographic: %d", 
              pszProjection ? pszProjection : "(null)",
              static_cast<int>(bIsProjected),
              static_cast<int>(bIsGeographic) );

    if ( ! bSetGeoTransform )
        CPLDebug( "GDAL_netCDF", "netCDFDataset::AddProjectionVars() called, "
                  "but GeoTransform has not yet been defined!" );

    if ( ! bSetProjection )
        CPLDebug( "GDAL_netCDF", "netCDFDataset::AddProjectionVars() called, "
                  "but Projection has not yet been defined!" );

    /* check GEOLOCATION information */
    char **papszGeolocationInfo = GetMetadata("GEOLOCATION");
    if ( papszGeolocationInfo != NULL ) {

        /* look for geolocation datasets */
        pszDSName = CSLFetchNameValue( papszGeolocationInfo, "X_DATASET" );
        if( pszDSName != NULL )
            hDS_X = GDALOpenShared( pszDSName, GA_ReadOnly );
        pszDSName = CSLFetchNameValue( papszGeolocationInfo, "Y_DATASET" );
        if( pszDSName != NULL )
            hDS_Y = GDALOpenShared( pszDSName, GA_ReadOnly );

        if ( hDS_X != NULL && hDS_Y != NULL ) {
            nBand = MAX(1,atoi(CSLFetchNameValueDef( papszGeolocationInfo,
                                                     "X_BAND", "0" )));
            hBand_X = GDALGetRasterBand( hDS_X, nBand );
            nBand = MAX(1,atoi(CSLFetchNameValueDef( papszGeolocationInfo,
                                                     "Y_BAND", "0" )));
            hBand_Y = GDALGetRasterBand( hDS_Y, nBand );

            // If geoloc bands are found, do basic validation based on their
            // dimensions.
            if ( hBand_X != NULL && hBand_Y != NULL ) {

                int nXSize_XBand = GDALGetRasterXSize( hDS_X );
                int nYSize_XBand = GDALGetRasterYSize( hDS_X );
                int nXSize_YBand = GDALGetRasterXSize( hDS_Y );
                int nYSize_YBand = GDALGetRasterYSize( hDS_Y );

                /* TODO 1D geolocation arrays not implemented */
                if ( (nYSize_XBand == 1) && (nYSize_YBand == 1) ) {
                    bHasGeoloc = false;
                    CPLDebug( "GDAL_netCDF",
                              "1D GEOLOCATION arrays not supported yet" );
                }
                /* 2D bands must have same sizes as the raster bands */
                else if ( (nXSize_XBand != nRasterXSize) ||
                          (nYSize_XBand != nRasterYSize) ||
                          (nXSize_YBand != nRasterXSize) ||
                          (nYSize_YBand != nRasterYSize) ) {
                    bHasGeoloc = false;
                    CPLDebug( "GDAL_netCDF",
                              "GEOLOCATION array sizes (%dx%d %dx%d) differ "
                              "from raster (%dx%d), not supported",
                              nXSize_XBand, nYSize_XBand, nXSize_YBand, nYSize_YBand,
                              nRasterXSize, nRasterYSize );
                }
                /* 2D bands are only supported for projected SRS (see CF 5.6) */
                else if ( ! bIsProjected ) {
                    bHasGeoloc = false;
                    CPLDebug( "GDAL_netCDF", 
                              "2D GEOLOCATION arrays only supported for projected SRS" );
                }
                else {
                    bHasGeoloc = true;
                    CPLDebug( "GDAL_netCDF", 
                              "dataset has GEOLOCATION information, will try to write it" );
                }
            }
        }
    }

    /* process projection options */
    if( bIsProjected ) 
    {
        bool bIsCfProjection = NCDFIsCfProjection( oSRS.GetAttrValue( "PROJECTION" ) );
        bWriteGridMapping = true;
        bWriteGDALTags = CPL_TO_BOOL(CSLFetchBoolean( papszCreationOptions, "WRITE_GDAL_TAGS", TRUE ));
        /* force WRITE_GDAL_TAGS if is not a CF projection */
        if ( ! bWriteGDALTags && ! bIsCfProjection )
            bWriteGDALTags = true;
        if ( bWriteGDALTags )
            bWriteGeoTransform = true;

        /* write lon/lat : default is NO, except if has geolocation */
        /* with IF_NEEDED : write if has geoloc or is not CF projection */ 
        pszValue = CSLFetchNameValue( papszCreationOptions,"WRITE_LONLAT" );
        if ( pszValue ) {
            if ( EQUAL( pszValue, "IF_NEEDED" ) ) {
                bWriteLonLat = ( bHasGeoloc || ! bIsCfProjection );
            }
            else bWriteLonLat = CPLTestBool( pszValue );
        }
        else
            bWriteLonLat = bHasGeoloc;

        /* save value of pszCFCoordinates for later */
        if ( bWriteLonLat ) {
            pszCFCoordinates = CPLStrdup( NCDF_LONLAT );
        }

        eLonLatType = NC_FLOAT;
        pszValue =  CSLFetchNameValueDef(papszCreationOptions,"TYPE_LONLAT", "FLOAT");
        if ( EQUAL(pszValue, "DOUBLE" ) ) 
            eLonLatType = NC_DOUBLE;
    }
    else
    {
        /* files without a Datum will not have a grid_mapping variable and geographic information */
        bWriteGridMapping = bIsGeographic;

        bWriteGDALTags = CPL_TO_BOOL(CSLFetchBoolean( papszCreationOptions, "WRITE_GDAL_TAGS", bWriteGridMapping ));
        if ( bWriteGDALTags )
            bWriteGeoTransform = true;

        pszValue =  CSLFetchNameValueDef(papszCreationOptions,"WRITE_LONLAT", "YES");
        if ( EQUAL( pszValue, "IF_NEEDED" ) )  
            bWriteLonLat = true;
        else
            bWriteLonLat = CPLTestBool( pszValue );
        /*  Don't write lon/lat if no source geotransform */
        if ( ! bSetGeoTransform )
            bWriteLonLat = false;
        /* If we don't write lon/lat, set dimnames to X/Y and write gdal tags*/
        if ( ! bWriteLonLat ) {
            CPLError( CE_Warning, CPLE_AppDefined, 
                      "creating geographic file without lon/lat values!");
            if ( bSetGeoTransform ) {
                bWriteGDALTags = true; // Not desirable if no geotransform.
                bWriteGeoTransform = true;
            }
        }

        eLonLatType = NC_DOUBLE;
        pszValue
            = CSLFetchNameValueDef(
                papszCreationOptions, "TYPE_LONLAT", "DOUBLE");
        if ( EQUAL(pszValue, "FLOAT" ) )
            eLonLatType = NC_FLOAT;
    }

    /* make sure we write grid_mapping if we need to write GDAL tags */
    if ( bWriteGDALTags ) bWriteGridMapping = true;

    /* bottom-up value: new driver is bottom-up by default */
    /* override with WRITE_BOTTOMUP */
    bBottomUp = CPL_TO_BOOL(CSLFetchBoolean( papszCreationOptions, "WRITE_BOTTOMUP", TRUE ));

    CPLDebug( "GDAL_netCDF", 
              "bIsProjected=%d bIsGeographic=%d bWriteGridMapping=%d "
              "bWriteGDALTags=%d bWriteLonLat=%d bBottomUp=%d bHasGeoloc=%d",
              static_cast<int>(bIsProjected),
              static_cast<int>(bIsGeographic),
              static_cast<int>(bWriteGridMapping),
              static_cast<int>(bWriteGDALTags),
              static_cast<int>(bWriteLonLat),
              static_cast<int>(bBottomUp),
              static_cast<int>(bHasGeoloc) );

    /* exit if nothing to do */
    if ( !bIsProjected && !bWriteLonLat )
        return CE_None;

/* -------------------------------------------------------------------- */
/*      Define dimension names                                          */
/* -------------------------------------------------------------------- */
    /* make sure we are in define mode */
    SetDefineMode( true );


/* -------------------------------------------------------------------- */
/*      Rename dimensions if lon/lat                                    */
/* -------------------------------------------------------------------- */
    if( ! bIsProjected ) 
    {
        /* rename dims to lat/lon */
        papszDimName.Clear(); //if we add other dims one day, this has to change
        papszDimName.AddString( NCDF_DIMNAME_LAT );
        papszDimName.AddString( NCDF_DIMNAME_LON );

        int status = nc_rename_dim(cdfid, nYDimID, NCDF_DIMNAME_LAT );
        NCDF_ERR(status);
        status = nc_rename_dim(cdfid, nXDimID, NCDF_DIMNAME_LON );
        NCDF_ERR(status);
    }

/* -------------------------------------------------------------------- */
/*      Write projection attributes                                     */
/* -------------------------------------------------------------------- */
    if( bWriteGridMapping )
    {
        NCDFVarID = NCDFWriteSRSVariable(cdfid, &oSRS, &pszCFProjection,
                                         bWriteGDALTags);
        if( NCDFVarID < 0 )
            return CE_Failure;

        /*  Optional GDAL custom projection tags */
        if ( bWriteGDALTags ) {
            CPLString osGeoTransform;
            for( int i=0; i<6; i++ ) {
                osGeoTransform += CPLSPrintf("%.16g ",
                         adfGeoTransform[i] );
            }
            CPLDebug( "GDAL_netCDF", "szGeoTransform = %s", osGeoTransform.c_str() );

            // if ( strlen(pszProj4Defn) > 0 ) {
            //     nc_put_att_text( cdfid, NCDFVarID, "proj4",
            //                      strlen( pszProj4Defn ), pszProj4Defn );
            // }

            /* for now write the geotransform for back-compat or else 
               the old (1.8.1) driver overrides the CF geotransform with 
               empty values from dfNN, dfSN, dfEE, dfWE; */
            /* TODO: fix this in 1.8 branch, and then remove this here */
            if ( bWriteGeoTransform && bSetGeoTransform ) {
                nc_put_att_text( cdfid, NCDFVarID, NCDF_GEOTRANSFORM,
                                 osGeoTransform.size(),
                                 osGeoTransform.c_str() );
            }
        }

        /* write projection variable to band variable */
        /* need to call later if there are no bands */
        AddGridMappingRef();

    }  /* end if( bWriteGridMapping ) */

    pfnProgress( 0.10, NULL, pProgressData );    

/* -------------------------------------------------------------------- */
/*      Write CF Projection vars                                        */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/*      Write X/Y attributes                                            */
/* -------------------------------------------------------------------- */
    if( bIsProjected )
    {
        /* X */
        int anXDims[1];
        anXDims[0] = nXDimID;
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d)",
                  cdfid, CF_PROJ_X_VAR_NAME, NC_DOUBLE );
        int status = nc_def_var( cdfid, CF_PROJ_X_VAR_NAME, NC_DOUBLE, 
                             1, anXDims, &nVarXID );
        NCDF_ERR(status);

        /* Y */
        int anYDims[1];
        anYDims[0] = nYDimID;
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d)",
                  cdfid, CF_PROJ_Y_VAR_NAME, NC_DOUBLE );
        status = nc_def_var( cdfid, CF_PROJ_Y_VAR_NAME, NC_DOUBLE, 
                             1, anYDims, &nVarYID );
        NCDF_ERR(status);

        NCDFWriteXYVarsAttributes(cdfid, nVarXID, nVarYID, &oSRS);
    }

/* -------------------------------------------------------------------- */
/*      Write lat/lon attributes if needed                              */
/* -------------------------------------------------------------------- */
    if ( bWriteLonLat ) {
        int *panLatDims=NULL;
        int *panLonDims=NULL;
        int nLatDims=-1;
        int nLonDims=-1;

        /* get information */
        if ( bHasGeoloc ) { /* geoloc */
            nLatDims = 2;
            panLatDims = (int *) CPLCalloc( nLatDims, sizeof( int ) );
            panLatDims[0] = nYDimID;
            panLatDims[1] = nXDimID;
            nLonDims = 2;
            panLonDims = (int *) CPLCalloc( nLonDims, sizeof( int ) );
            panLonDims[0] = nYDimID;
            panLonDims[1] = nXDimID;
        }
        else if ( bIsProjected ) { /* projected */
            nLatDims = 2;
            panLatDims = (int *) CPLCalloc( nLatDims, sizeof( int ) );
            panLatDims[0] =  nYDimID;
            panLatDims[1] =  nXDimID;
            nLonDims = 2;
            panLonDims = (int *) CPLCalloc( nLonDims, sizeof( int ) );
            panLonDims[0] =  nYDimID;
            panLonDims[1] =  nXDimID;
        }
        else {  /* geographic */
            nLatDims = 1;
            panLatDims = (int *) CPLCalloc( nLatDims, sizeof( int ) );
            panLatDims[0] = nYDimID;
            nLonDims = 1;
            panLonDims = (int *) CPLCalloc( nLonDims, sizeof( int ) );
            panLonDims[0] = nXDimID;
        }

        /* def vars and attributes */
        int status = nc_def_var( cdfid, CF_LATITUDE_VAR_NAME, eLonLatType, 
                             nLatDims, panLatDims, &nVarLatID );
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d,%d,-,-) got id %d",
                  cdfid, CF_LATITUDE_VAR_NAME, eLonLatType, nLatDims, nVarLatID );
        NCDF_ERR(status);
        DefVarDeflate( nVarLatID, false ); // don't set chunking

        status = nc_def_var( cdfid, CF_LONGITUDE_VAR_NAME, eLonLatType, 
                             nLonDims, panLonDims, &nVarLonID );
        CPLDebug( "GDAL_netCDF", "nc_def_var(%d,%s,%d,%d,-,-) got id %d",
                  cdfid, CF_LONGITUDE_VAR_NAME, eLonLatType, nLatDims, nVarLonID );
        NCDF_ERR(status);
        DefVarDeflate( nVarLonID, false ); // don't set chunking

        NCDFWriteLonLatVarsAttributes(cdfid, nVarLonID, nVarLatID);

        /* free data */
        CPLFree( panLatDims );
        CPLFree( panLonDims );
    }

/* -------------------------------------------------------------------- */
/*      Get projection values                                           */
/* -------------------------------------------------------------------- */

    double dfX0 = 0.0;
    double dfDX = 0.0;
    double dfY0 = 0.0;
    double dfDY = 0.0;
    double *padLonVal = NULL;
    double *padLatVal = NULL; /* should use float for projected, save space */

    if( bIsProjected )
    {
        // const char  *pszProjection;
        OGRSpatialReference *poLatLonSRS = NULL;
        OGRCoordinateTransformation *poTransform = NULL;

        char *pszWKT2 = (char *) pszProjection;
        OGRSpatialReference oSRS2;
        oSRS2.importFromWkt( &pszWKT2 );

        double *padYVal = NULL;
        double *padXVal = NULL;
        size_t startX[1];
        size_t countX[1];
        size_t startY[1];
        size_t countY[1];

        CPLDebug("GDAL_netCDF", "Getting (X,Y) values" );

        padXVal = (double *) CPLMalloc( nRasterXSize * sizeof( double ) );
        padYVal = (double *) CPLMalloc( nRasterYSize * sizeof( double ) );

/* -------------------------------------------------------------------- */
/*      Get Y values                                                    */
/* -------------------------------------------------------------------- */
        if ( ! bBottomUp )
            dfY0 = adfGeoTransform[3];
        else /* invert latitude values */ 
            dfY0 = adfGeoTransform[3] + ( adfGeoTransform[5] * nRasterYSize );
        dfDY = adfGeoTransform[5];

        for( int j=0; j<nRasterYSize; j++ ) {
            /* The data point is centered inside the pixel */
            if ( ! bBottomUp )
                padYVal[j] = dfY0 + (j+0.5)*dfDY ;
            else /* invert latitude values */ 
                padYVal[j] = dfY0 - (j+0.5)*dfDY ;
        }
        startX[0] = 0;
        countX[0] = nRasterXSize;

/* -------------------------------------------------------------------- */
/*      Get X values                                                    */
/* -------------------------------------------------------------------- */
        dfX0 = adfGeoTransform[0];
        dfDX = adfGeoTransform[1];

        for( int i=0; i<nRasterXSize; i++ ) {
            /* The data point is centered inside the pixel */
            padXVal[i] = dfX0 + (i+0.5)*dfDX ;
        }
        startY[0] = 0;
        countY[0] = nRasterYSize;

/* -------------------------------------------------------------------- */
/*      Write X/Y values                                                */
/* -------------------------------------------------------------------- */
        /* make sure we are in data mode */
        SetDefineMode( false );

        CPLDebug("GDAL_netCDF", "Writing X values" );
        int status = nc_put_vara_double( cdfid, nVarXID, startX,
                                     countX, padXVal);
        NCDF_ERR(status);

        CPLDebug("GDAL_netCDF", "Writing Y values" );
        status = nc_put_vara_double( cdfid, nVarYID, startY,
                                     countY, padYVal);
        NCDF_ERR(status);

        pfnProgress( 0.20, NULL, pProgressData );


/* -------------------------------------------------------------------- */
/*      Write lon/lat arrays (CF coordinates) if requested              */
/* -------------------------------------------------------------------- */

        /* Get OGR transform if GEOLOCATION is not available */
        if ( bWriteLonLat && !bHasGeoloc ) {
            poLatLonSRS = oSRS2.CloneGeogCS();
            if ( poLatLonSRS != NULL )
                poTransform = OGRCreateCoordinateTransformation( &oSRS2, poLatLonSRS );
            /* if no OGR transform, then don't write CF lon/lat */
            if( poTransform == NULL ) {
                CPLError( CE_Failure, CPLE_AppDefined, 
                          "Unable to get Coordinate Transform" );
                bWriteLonLat = false;
            }
        }

        if ( bWriteLonLat )  {

            if ( ! bHasGeoloc )
                CPLDebug("GDAL_netCDF", "Transforming (X,Y)->(lon,lat)" );
            else
                CPLDebug("GDAL_netCDF", "writing (lon,lat) from GEOLOCATION arrays" );

            bool bOK = true;
            double dfProgress = 0.2;

            size_t start[]={ 0, 0 };
            size_t count[]={ 1, (size_t)nRasterXSize };
            padLatVal = (double *) CPLMalloc( nRasterXSize * sizeof( double ) );
            padLonVal = (double *) CPLMalloc( nRasterXSize * sizeof( double ) );

            for( int j = 0; (j < nRasterYSize) && bOK && (status == NC_NOERR); j++ ) {

                start[0] = j;

                /* get values from geotransform */
                if ( ! bHasGeoloc ) {
                    /* fill values to transform */
                    for( int i=0; i<nRasterXSize; i++ ) {
                        padLatVal[i] = padYVal[j];
                        padLonVal[i] = padXVal[i];
                    }

                    /* do the transform */
                    bOK = CPL_TO_BOOL(poTransform->Transform( nRasterXSize, 
                                                  padLonVal, padLatVal, NULL ));
                    if ( ! bOK ) {
                        CPLError( CE_Failure, CPLE_AppDefined, 
                                  "Unable to Transform (X,Y) to (lon,lat).\n" );
                    }
                }
                /* get values from geoloc arrays */
                else {
                    eErr = GDALRasterIO( hBand_Y, GF_Read, 
                                         0, j, nRasterXSize, 1,
                                         padLatVal, nRasterXSize, 1, 
                                         GDT_Float64, 0, 0 );
                    if ( eErr == CE_None ) {
                        eErr = GDALRasterIO( hBand_X, GF_Read, 
                                             0, j, nRasterXSize, 1,
                                             padLonVal, nRasterXSize, 1, 
                                             GDT_Float64, 0, 0 );
                    }

                    if ( eErr == CE_None )
                        bOK = true;
                    else {
                        bOK = false;
                        CPLError( CE_Failure, CPLE_AppDefined, 
                                  "Unable to get scanline %d\n",j );
                    }
                }

                /* write data */
                if ( bOK ) {
                    status = nc_put_vara_double( cdfid, nVarLatID, start,
                                                 count, padLatVal);
                    NCDF_ERR(status);
                    status = nc_put_vara_double( cdfid, nVarLonID, start,
                                                 count, padLonVal);
                    NCDF_ERR(status);
                }

                if ( (nRasterYSize/10) >0 && (j % (nRasterYSize/10) == 0) ) {
                    dfProgress += 0.08;
                    pfnProgress( dfProgress , NULL, pProgressData );
                }
            }

        }

        /* Free the srs and transform objects */
        if ( poLatLonSRS != NULL ) delete poLatLonSRS;
        if ( poTransform != NULL ) delete poTransform;

        /* Free data */
        CPLFree( padXVal );
        CPLFree( padYVal );
        CPLFree( padLonVal );
        CPLFree( padLatVal);

    } // projected

    /* If not Projected assume Geographic to catch grids without Datum */
    else if ( bWriteLonLat )  {

/* -------------------------------------------------------------------- */
/*      Get latitude values                                             */
/* -------------------------------------------------------------------- */
        if ( ! bBottomUp )
            dfY0 = adfGeoTransform[3];
        else /* invert latitude values */ 
            dfY0 = adfGeoTransform[3] + ( adfGeoTransform[5] * nRasterYSize );
        dfDY = adfGeoTransform[5];

        /* override lat values with the ones in GEOLOCATION/Y_VALUES */
        if ( GetMetadataItem( "Y_VALUES", "GEOLOCATION" ) != NULL ) {
            int nTemp = 0;
            padLatVal = Get1DGeolocation( "Y_VALUES", nTemp );
            /* make sure we got the correct amount, if not fallback to GT */
            /* could add test fabs( fabs(padLatVal[0]) - fabs(dfY0) ) <= 0.1 ) ) */
            if ( nTemp == nRasterYSize ) {
                CPLDebug("GDAL_netCDF", "Using Y_VALUES geolocation metadata for lat values" );
            }
            else {
                CPLDebug("GDAL_netCDF", 
                         "Got %d elements from Y_VALUES geolocation metadata, need %d",
                         nTemp, nRasterYSize );
                if ( padLatVal ) {
                    CPLFree( padLatVal );
                    padLatVal = NULL;
                } 
            }
        }

        if ( padLatVal == NULL ) {
            padLatVal = (double *) CPLMalloc( nRasterYSize * sizeof( double ) );
            for( int i=0; i<nRasterYSize; i++ ) {
                /* The data point is centered inside the pixel */
                if ( ! bBottomUp )
                    padLatVal[i] = dfY0 + (i+0.5)*dfDY ;
                else /* invert latitude values */ 
                    padLatVal[i] = dfY0 - (i+0.5)*dfDY ;
            }
        }

        size_t startLat[1] = {0};
        size_t countLat[1] = {static_cast<size_t>(nRasterYSize)};

/* -------------------------------------------------------------------- */
/*      Get longitude values                                            */
/* -------------------------------------------------------------------- */
        dfX0 = adfGeoTransform[0];
        dfDX = adfGeoTransform[1];

        padLonVal = (double *) CPLMalloc( nRasterXSize * sizeof( double ) );
        for( int i=0; i<nRasterXSize; i++ ) {
            /* The data point is centered inside the pixel */
            padLonVal[i] = dfX0 + (i+0.5)*dfDX ;
        }

        size_t startLon[1] = {0};
        size_t countLon[1] = {static_cast<size_t>(nRasterXSize)};

/* -------------------------------------------------------------------- */
/*      Write latitude and longitude values                             */
/* -------------------------------------------------------------------- */
        /* make sure we are in data mode */
        SetDefineMode( false );

        /* write values */
        CPLDebug("GDAL_netCDF", "Writing lat values" );

        int status = nc_put_vara_double( cdfid, nVarLatID, startLat,
                                     countLat, padLatVal);
        NCDF_ERR(status);

        CPLDebug("GDAL_netCDF", "Writing lon values" );
        status = nc_put_vara_double( cdfid, nVarLonID, startLon,
                                     countLon, padLonVal);
        NCDF_ERR(status);

        /* free values */
        CPLFree( padLatVal );  
        CPLFree( padLonVal );  
    }// not projected 

    /* close geoloc datasets */
    if( hDS_X != NULL ) {
        GDALClose( hDS_X ); 
    }
    if( hDS_Y != NULL ) {
        GDALClose( hDS_Y ); 
    }

    pfnProgress( 1.00, NULL, pProgressData );

    return CE_None;
}

/* Write Projection variable to band variable */
/* Moved from AddProjectionVars() for cases when bands are added after projection */
void netCDFDataset::AddGridMappingRef( )
{
    int nVarId = -1;
    bool bOldDefineMode = bDefineMode;

    if( (GetAccess() == GA_Update) && 
        (nBands >= 1) && (GetRasterBand( 1 )) &&
        pszCFProjection != NULL && ! EQUAL( pszCFProjection, "" ) ) {

        nVarId = ( (netCDFRasterBand *) GetRasterBand( 1 ) )->nZId;
        bAddedGridMappingRef = true;

        /* make sure we are in define mode */
        SetDefineMode( true );
        int status = nc_put_att_text( cdfid, nVarId, 
                                  CF_GRD_MAPPING,
                                  strlen( pszCFProjection ),
                                  pszCFProjection );
        NCDF_ERR(status);
        if ( pszCFCoordinates != NULL && ! EQUAL( pszCFCoordinates, "" ) ) {
            status = nc_put_att_text( cdfid, nVarId, 
                                      CF_COORDINATES,
                                      strlen( pszCFCoordinates ), 
                                      pszCFCoordinates );
            NCDF_ERR(status);
        }

        /* go back to previous define mode */
        SetDefineMode( bOldDefineMode );
    }
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr netCDFDataset::GetGeoTransform( double * padfTransform )

{
    memcpy( padfTransform, adfGeoTransform, sizeof(double) * 6 );
    if( bSetGeoTransform )
        return CE_None;

    return GDALPamDataset::GetGeoTransform( padfTransform );
}

/************************************************************************/
/*                                rint()                                */
/************************************************************************/

double netCDFDataset::rint( double dfX)
{
    if( dfX > 0 ) {
        int nX = (int) (dfX+0.5);
        if( nX % 2 ) {
            double dfDiff = dfX - (double)nX;
            if( dfDiff == -0.5 )
                return double( nX-1 );
        }
        return double( nX );
    } else {
        int nX = (int) (dfX-0.5);
        if( nX % 2 ) {
            double dfDiff = dfX - (double)nX;
            if( dfDiff == 0.5 )
                return double(nX+1);
        }
        return double(nX);
    }
}

/************************************************************************/
/*                        ReadAttributes()                              */
/************************************************************************/
CPLErr netCDFDataset::ReadAttributes( int cdfidIn, int var)

{
    char    szVarName [ NC_MAX_NAME+1 ];
    int     nbAttr;

    nc_inq_varnatts( cdfidIn, var, &nbAttr );
    if( var == NC_GLOBAL ) {
        strcpy( szVarName, "NC_GLOBAL" );
    }
    else {
        szVarName[0] = '\0';
        int status = nc_inq_varname( cdfid, var, szVarName );
        NCDF_ERR(status);
    }

    for( int l=0; l < nbAttr; l++) {
        char szAttrName[ NC_MAX_NAME+1 ];
        szAttrName[0] = 0;
        int status = nc_inq_attname( cdfid, var, l, szAttrName);
        NCDF_ERR(status);
        char szMetaName[ NC_MAX_NAME * 2 + 1 + 1 ];
        snprintf( szMetaName, sizeof(szMetaName), "%s#%s", szVarName, szAttrName  );

        char *pszMetaTemp = NULL;
        if ( NCDFGetAttr( cdfidIn, var, szAttrName, &pszMetaTemp )
             == CE_None ) {
            papszMetadata = CSLSetNameValue(papszMetadata, 
                                            szMetaName, 
                                            pszMetaTemp);
            CPLFree(pszMetaTemp);
            pszMetaTemp = NULL;
        }
        else {
            CPLDebug( "GDAL_netCDF", "invalid global metadata %s", szMetaName );
        }
    }

    return CE_None;
}


/************************************************************************/
/*                netCDFDataset::CreateSubDatasetList()                 */
/************************************************************************/
void netCDFDataset::CreateSubDatasetList( )
{
    char         szName[ NC_MAX_NAME+1 ];
    char         szVarStdName[ NC_MAX_NAME+1 ];
    int          *ponDimIds;
    nc_type      nAttype;
    size_t       nAttlen;

    netCDFDataset *poDS = this;

    int nSub = 1;
    int nVarCount;
    nc_inq_nvars ( cdfid, &nVarCount );

    for ( int nVar = 0; nVar < nVarCount; nVar++ ) {

        int nDims;
        nc_inq_varndims ( cdfid, nVar, &nDims );

        if( nDims >= 2 ) {
            ponDimIds = (int *) CPLCalloc( nDims, sizeof( int ) );
            nc_inq_vardimid ( cdfid, nVar, ponDimIds );

/* -------------------------------------------------------------------- */
/*      Create Sub dataset list                                         */
/* -------------------------------------------------------------------- */
            CPLString osDim;
            for( int i = 0; i < nDims; i++ ) {
                size_t nDimLen;
                nc_inq_dimlen ( cdfid, ponDimIds[i], &nDimLen );
                osDim += CPLSPrintf("%dx", (int) nDimLen);
            }

            nc_type nVarType;
            nc_inq_vartype( cdfid, nVar, &nVarType );
/* -------------------------------------------------------------------- */
/*      Get rid of the last "x" character                               */
/* -------------------------------------------------------------------- */
            osDim.resize(osDim.size()-1);
            const char* pszType = "";
            switch( nVarType ) {
                case NC_BYTE:
                    pszType = "8-bit integer";
                    break;
                case NC_CHAR:
                    pszType = "8-bit character";
                    break;
                case NC_SHORT:
                    pszType = "16-bit integer";
                    break;
                case NC_INT:
                    pszType = "32-bit integer";
                    break;
                case NC_FLOAT:
                    pszType = "32-bit floating-point";
                    break;
                case NC_DOUBLE:
                    pszType = "64-bit floating-point";
                    break;
#ifdef NETCDF_HAS_NC4
                case NC_UBYTE:
                    pszType = "8-bit unsigned integer";
                    break;
                case NC_USHORT:
                    pszType = "16-bit unsigned integer";
                    break;
                case NC_UINT:
                    pszType = "32-bit unsigned integer";
                    break;
                case NC_INT64:
                    pszType = "64-bit integer";
                    break;
                case NC_UINT64:
                    pszType = "64-bit unsigned integer";
                    break;
#endif
                default:
                    break;
            }
            szName[0] = '\0';
            int status = nc_inq_varname( cdfid, nVar, szName);
            NCDF_ERR(status);
            nAttlen = 0;
            nc_inq_att( cdfid, nVar, CF_STD_NAME, &nAttype, &nAttlen);
            if( nAttlen < sizeof(szVarStdName) &&
                nc_get_att_text ( cdfid, nVar, CF_STD_NAME, 
                                  szVarStdName ) == NC_NOERR ) {
                szVarStdName[nAttlen] = '\0';
            }
            else {
                snprintf( szVarStdName, sizeof(szVarStdName), "%s", szName );
            }

            char szTemp[ NC_MAX_NAME+1 ];
            snprintf( szTemp, sizeof(szTemp), "SUBDATASET_%d_NAME", nSub);

            poDS->papszSubDatasets =
                CSLSetNameValue( poDS->papszSubDatasets, szTemp,
                                 CPLSPrintf( "NETCDF:\"%s\":%s",
                                             poDS->osFilename.c_str(),
                                             szName)  ) ;

            snprintf( szTemp, sizeof(szTemp), "SUBDATASET_%d_DESC", nSub++ );

            poDS->papszSubDatasets =
                CSLSetNameValue( poDS->papszSubDatasets, szTemp,
                                 CPLSPrintf( "[%s] %s (%s)", 
                                             osDim.c_str(),
                                             szVarStdName,
                                             pszType ) );

            CPLFree(ponDimIds);
        }
    }
}

/************************************************************************/
/*                              IdentifyFormat()                      */
/************************************************************************/

NetCDFFormatEnum netCDFDataset::IdentifyFormat( GDALOpenInfo * poOpenInfo, 
#ifndef HAVE_HDF5
CPL_UNUSED
#endif
                                   bool bCheckExt = true )
{
/* -------------------------------------------------------------------- */
/*      Does this appear to be a netcdf file? If so, which format?      */
/*      http://www.unidata.ucar.edu/software/netcdf/docs/faq.html#fv1_5 */
/* -------------------------------------------------------------------- */

    if( STARTS_WITH_CI(poOpenInfo->pszFilename, "NETCDF:") )
        return NCDF_FORMAT_UNKNOWN;
    if ( poOpenInfo->nHeaderBytes < 4 )
        return NCDF_FORMAT_NONE;
    if ( STARTS_WITH_CI((char*)poOpenInfo->pabyHeader, "CDF\001") )
    {
        /* In case the netCDF driver is registered before the GMT driver, */
        /* avoid opening GMT files */
        if( GDALGetDriverByName("GMT") != NULL )
        {
            bool bFoundZ = false;
            bool bFoundDimension = false;
            for(int i=0;i<poOpenInfo->nHeaderBytes - 11;i++)
            {
                if( poOpenInfo->pabyHeader[i] == 1 &&
                    poOpenInfo->pabyHeader[i+1] == 'z' &&
                    poOpenInfo->pabyHeader[i+2] == 0  )
                    bFoundZ = true;
                else if( poOpenInfo->pabyHeader[i] == 9 &&
                        memcmp((const char*)poOpenInfo->pabyHeader + i + 1, "dimension", 9) == 0 &&
                        poOpenInfo->pabyHeader[i+10] == 0 )
                    bFoundDimension = true;
            }
            if( bFoundZ && bFoundDimension )
                return NCDF_FORMAT_UNKNOWN;
        }

        return NCDF_FORMAT_NC;
    }
    else if ( STARTS_WITH_CI((char*)poOpenInfo->pabyHeader, "CDF\002") )
        return NCDF_FORMAT_NC2;
    else if ( STARTS_WITH_CI((char*)poOpenInfo->pabyHeader, "\211HDF\r\n\032\n") ) {
        /* Requires netCDF-4/HDF5 support in libnetcdf (not just libnetcdf-v4).
           If HDF5 is not supported in GDAL, this driver will try to open the file 
           Else, make sure this driver does not try to open HDF5 files 
           If user really wants to open with this driver, use NETCDF:file.h5 format. 
           This check should be relaxed, but there is no clear way to make a difference. 
        */

        /* Check for HDF5 support in GDAL */
#ifdef HAVE_HDF5
        if ( bCheckExt ) { /* Check by default */
            const char* pszExtension = CPLGetExtension( poOpenInfo->pszFilename );
            if ( ! ( EQUAL( pszExtension, "nc")  || EQUAL( pszExtension, "cdf") 
                     || EQUAL( pszExtension, "nc2") || EQUAL( pszExtension, "nc4")
					 || EQUAL( pszExtension, "nc3") || EQUAL( pszExtension, "grd") ) )
                return NCDF_FORMAT_HDF5;
        }
#endif

        /* Check for netcdf-4 support in libnetcdf */
#ifdef NETCDF_HAS_NC4
        return NCDF_FORMAT_NC4;
#else
        return NCDF_FORMAT_HDF5;
#endif

    }
    else if ( STARTS_WITH_CI((char*)poOpenInfo->pabyHeader, "\016\003\023\001") ) {
        /* Requires HDF4 support in libnetcdf, but if HF4 is supported by GDAL don't try to open. */
        /* If user really wants to open with this driver, use NETCDF:file.hdf syntax. */

        /* Check for HDF4 support in GDAL */
#ifdef HAVE_HDF4
        if ( bCheckExt ) { /* Check by default */
            /* Always treat as HDF4 file */
            return NCDF_FORMAT_HDF4;
        }
#endif

        /* Check for HDF4 support in libnetcdf */
#ifdef NETCDF_HAS_HDF4
        return NCDF_FORMAT_NC4; 
#else
        return NCDF_FORMAT_HDF4;
#endif
    }

    return NCDF_FORMAT_NONE;
}

/************************************************************************/
/*                            TestCapability()                          */
/************************************************************************/

int netCDFDataset::TestCapability(const char* pszCap)
{
    if( EQUAL(pszCap, ODsCCreateLayer) )
        return eAccess == GA_Update && nBands == 0 && nLayers == 0;
    return FALSE;
}

/************************************************************************/
/*                            GetLayer()                                */
/************************************************************************/

OGRLayer* netCDFDataset::GetLayer(int nIdx)
{
    if( nIdx < 0 || nIdx >= nLayers )
        return NULL;
    return papoLayers[nIdx];
}

/************************************************************************/
/*                            ICreateLayer()                            */
/************************************************************************/

OGRLayer* netCDFDataset::ICreateLayer( const char *pszName,
                                       OGRSpatialReference *poSpatialRef,
                                       OGRwkbGeometryType eGType,
                                       char ** papszOptions )
{
    if( !TestCapability(ODsCCreateLayer) )
        return NULL;

    netCDFLayer* poLayer = new netCDFLayer(this, pszName, eGType, poSpatialRef);
    if( !poLayer->Create(papszOptions) )
    {
        delete poLayer;
        return NULL;
    }
    papoLayers = static_cast<OGRLayer**>(CPLRealloc(papoLayers, (nLayers + 1) * sizeof(OGRLayer)));
    papoLayers[nLayers++] = poLayer;
    return poLayer;
}

/************************************************************************/
/*                            netCDFLayer()                             */
/************************************************************************/

netCDFLayer::netCDFLayer(netCDFDataset* poDS,
                         const char* pszName,
                         OGRwkbGeometryType eGeomType,
                         OGRSpatialReference* poSRS) :
        m_poDS(poDS),
        m_poFeatureDefn(new OGRFeatureDefn(pszName)),
        m_osRecordDimName("record"),
        m_nRecordDimID(-1),
        m_nDefaultMaxWidth(80),
        m_nDefaultMaxWidthDimId(-1),
        m_nXVarID(-1),
        m_nYVarID(-1),
        m_nZVarID(-1),
        m_nXVarNCDFType(NC_NAT),
        m_nYVarNCDFType(NC_NAT),
        m_nZVarNCDFType(NC_NAT),
        m_osWKTVarName("wkt"),
        m_nWKTMaxWidth(10000),
        m_nWKTMaxWidthDimId(-1),
        m_nWKTVarID(-1),
        m_nWKTNCDFType(NC_NAT),
        m_nCurFeatureId(1),
        m_pszCFProjection(NULL),
        m_bWriteGDALTags(true),
        m_bUseStringInNC4(true)
{
    m_uXVarNoData.nVal64 = 0;
    m_uYVarNoData.nVal64 = 0;
    m_uZVarNoData.nVal64 = 0;
    m_poFeatureDefn->SetGeomType(eGeomType);
    if( eGeomType != wkbNone )
        m_poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(poSRS);
    m_poFeatureDefn->Reference();
    SetDescription(pszName);
}

/************************************************************************/
/*                           ~netCDFLayer()                             */
/************************************************************************/

netCDFLayer::~netCDFLayer()
{
    m_poFeatureDefn->Release();
    CPLFree(m_pszCFProjection);
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

bool netCDFLayer::Create(char** papszOptions)
{
    m_osRecordDimName = CSLFetchNameValueDef(papszOptions, "RECORD_DIM_NAME",
                                             m_osRecordDimName.c_str());
    m_nDefaultMaxWidth = atoi(CSLFetchNameValueDef(papszOptions,
                                                   "STRING_MAX_WIDTH", "80"));
    m_bWriteGDALTags = CPL_TO_BOOL(CSLFetchBoolean(
                    m_poDS->papszCreationOptions, "WRITE_GDAL_TAGS", TRUE ));
    m_bUseStringInNC4 = CPL_TO_BOOL(CSLFetchBoolean(
                                papszOptions, "USE_STRING_IN_NC4", TRUE ));

    int status;
    if( m_bWriteGDALTags )
    {
        status = nc_put_att_text( m_poDS->GetCDFID(), NC_GLOBAL, "ogr_layer_name",
                        strlen(m_poFeatureDefn->GetName()), m_poFeatureDefn->GetName());
        NCDF_ERR(status);
    }

    status = nc_def_dim( m_poDS->GetCDFID(), m_osRecordDimName,
                             NC_UNLIMITED, &m_nRecordDimID );
    NCDF_ERR(status);
    if( status != NC_NOERR ) 
        return false;

    OGRSpatialReference* poSRS = NULL;
    if( m_poFeatureDefn->GetGeomFieldCount() )
        poSRS = m_poFeatureDefn->GetGeomFieldDefn(0)->GetSpatialRef();

    if( wkbFlatten(m_poFeatureDefn->GetGeomType()) == wkbPoint )
    {
        const bool bIsGeographic = (poSRS == NULL || poSRS->IsGeographic());

        const char* pszXVarName =
                bIsGeographic ? CF_LONGITUDE_VAR_NAME : CF_PROJ_X_VAR_NAME;
        status = nc_def_var( m_poDS->GetCDFID(),
                             pszXVarName,
                             NC_DOUBLE, 1, &m_nRecordDimID, &m_nXVarID);
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return false;
        }

        const char* pszYVarName =
                bIsGeographic ? CF_LATITUDE_VAR_NAME : CF_PROJ_Y_VAR_NAME;
        status = nc_def_var( m_poDS->GetCDFID(),
                             pszYVarName,
                             NC_DOUBLE, 1, &m_nRecordDimID, &m_nYVarID);
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return false;
        }

        m_nXVarNCDFType = NC_DOUBLE;
        m_nYVarNCDFType = NC_DOUBLE;
        m_uXVarNoData.dfVal = NC_FILL_DOUBLE;
        m_uYVarNoData.dfVal = NC_FILL_DOUBLE;

        m_osCoordinatesValue = pszXVarName;
        m_osCoordinatesValue += " ";
        m_osCoordinatesValue += pszYVarName;

        if (poSRS && poSRS->IsGeographic() )
        {
            NCDFWriteLonLatVarsAttributes(m_poDS->GetCDFID(), m_nXVarID, m_nYVarID);
        }
        else if (poSRS && poSRS->IsProjected() )
        {
            NCDFWriteXYVarsAttributes(m_poDS->GetCDFID(), m_nXVarID, m_nYVarID, poSRS);
        }

        if( m_poFeatureDefn->GetGeomType() == wkbPoint25D )
        {
            const char* pszZVarName = "z";
            status = nc_def_var( m_poDS->GetCDFID(),
                                 pszZVarName, NC_DOUBLE,
                                 1, &m_nRecordDimID, &m_nZVarID);
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return false;
            }

            m_nZVarNCDFType = NC_DOUBLE;
            m_uZVarNoData.dfVal = NC_FILL_DOUBLE;

            status = nc_put_att_text( m_poDS->GetCDFID(), m_nZVarID, CF_LNG_NAME,
                             strlen("z coordinate"), "z coordinate");
            NCDF_ERR(status);

            status = nc_put_att_text( m_poDS->GetCDFID(), m_nZVarID, CF_STD_NAME,
                             strlen("height"), "height");
            NCDF_ERR(status);

            status = nc_put_att_text( m_poDS->GetCDFID(), m_nZVarID, CF_AXIS,
                             strlen("Z"), "Z");
            NCDF_ERR(status);

            status = nc_put_att_text( m_poDS->GetCDFID(), m_nZVarID, CF_UNITS,
                             strlen("m"), "m");
            NCDF_ERR(status);

            m_osCoordinatesValue += " ";
            m_osCoordinatesValue += pszZVarName;

        }

        status = nc_put_att_text( m_poDS->GetCDFID(), NC_GLOBAL, "featureType",
                         strlen("point"), "point");
        NCDF_ERR(status);
    }
    else if( m_poFeatureDefn->GetGeomType() != wkbNone )
    {
#ifdef NETCDF_HAS_NC4
        if( m_poDS->eFormat == NCDF_FORMAT_NC4 && m_bUseStringInNC4 )
        {
            m_nWKTNCDFType = NC_STRING;
            status = nc_def_var( m_poDS->GetCDFID(),
                                 m_osWKTVarName.c_str(), NC_STRING, 1,
                                 &m_nRecordDimID, &m_nWKTVarID);
        }
        else
#endif
        {
            m_nWKTNCDFType = NC_CHAR;
            m_nWKTMaxWidth = atoi(CSLFetchNameValueDef(papszOptions, "WKT_MAX_WIDTH",
                                                       CPLSPrintf("%d", m_nWKTMaxWidth)));
            status = nc_def_dim( m_poDS->GetCDFID(),
                                 CPLSPrintf("%s_max_width", m_osWKTVarName.c_str()),
                                 m_nWKTMaxWidth,
                                 &m_nWKTMaxWidthDimId );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return false;
            }

            int anDims[] = { m_nRecordDimID, m_nWKTMaxWidthDimId };
            status = nc_def_var( m_poDS->GetCDFID(),
                                 m_osWKTVarName.c_str(), NC_CHAR, 2,
                                 anDims, &m_nWKTVarID );
        }
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return false;
        }

        status = nc_put_att_text( m_poDS->GetCDFID(), m_nWKTVarID, CF_LNG_NAME,
                         strlen("Geometry as ISO WKT"), "Geometry as ISO WKT" );
        NCDF_ERR(status);

        //nc_put_att_text( m_poDS->GetCDFID(), m_nWKTVarID, CF_UNITS,
        //                 strlen("none"), "none" );

        if( m_bWriteGDALTags )
        {
            status = nc_put_att_text( m_poDS->GetCDFID(), NC_GLOBAL, "ogr_geometry_field",
                             m_osWKTVarName.size(), m_osWKTVarName.c_str() );
            NCDF_ERR(status);

            CPLString osGeometryType = OGRToOGCGeomType(m_poFeatureDefn->GetGeomType());
            if( wkbHasZ(m_poFeatureDefn->GetGeomType()) )
                osGeometryType += " Z";
            status = nc_put_att_text( m_poDS->GetCDFID(), NC_GLOBAL, "ogr_layer_type",
                                      osGeometryType.size(), osGeometryType.c_str());
            NCDF_ERR(status);
        }
    }

    if( poSRS != NULL )
    {
        int nSRSVarId = NCDFWriteSRSVariable(m_poDS->GetCDFID(), poSRS,
                                             &m_pszCFProjection,
                                             m_bWriteGDALTags);
        if( nSRSVarId < 0 )
            return false;

        if( m_nWKTVarID >= 0 && m_pszCFProjection != NULL )
        {
            status = nc_put_att_text( m_poDS->GetCDFID(), m_nWKTVarID, CF_GRD_MAPPING, 
                            strlen(m_pszCFProjection), m_pszCFProjection );
            NCDF_ERR(status);
        }
    }

    return true;
}

/************************************************************************/
/*                          SetRecordDimID()                            */
/************************************************************************/

void netCDFLayer::SetRecordDimID(int nRecordDimID)
{
    m_nRecordDimID = nRecordDimID;
    char szTemp[NC_MAX_NAME+1];
    szTemp[0] = 0;
    nc_inq_dimname( m_poDS->GetCDFID(), m_nRecordDimID, szTemp);
    m_osRecordDimName = szTemp;
}


/************************************************************************/
/*                            GetFillValue()                            */
/************************************************************************/

CPLErr netCDFLayer::GetFillValue( int nVarId, char **ppszValue )
{
    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarId, _FillValue, ppszValue) == CE_None )
        return CE_None;
    return NCDFGetAttr( m_poDS->GetCDFID(), nVarId, "missing_value", ppszValue);
}

CPLErr netCDFLayer::GetFillValue( int nVarId, double *pdfValue )
{
    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarId, _FillValue, pdfValue) == CE_None )
        return CE_None;
    return NCDFGetAttr( m_poDS->GetCDFID(), nVarId, "missing_value", pdfValue);
}

/************************************************************************/
/*                         GetNoDataValueForFloat()                     */
/************************************************************************/

void netCDFLayer::GetNoDataValueForFloat( int nVarId, NCDFNoDataUnion* puNoData )
{
    double dfValue;
    if( GetFillValue( nVarId, &dfValue) == CE_None )
        puNoData->fVal = static_cast<float>(dfValue);
    else
        puNoData->fVal = NC_FILL_FLOAT;
}

/************************************************************************/
/*                        GetNoDataValueForDouble()                     */
/************************************************************************/

void netCDFLayer::GetNoDataValueForDouble( int nVarId, NCDFNoDataUnion* puNoData )
{
    double dfValue;
    if( GetFillValue( nVarId, &dfValue) == CE_None )
        puNoData->dfVal = dfValue;
    else
        puNoData->dfVal = NC_FILL_DOUBLE;
}

/************************************************************************/
/*                            GetNoDataValue()                          */
/************************************************************************/

void netCDFLayer::GetNoDataValue( int nVarId, nc_type nVarType, NCDFNoDataUnion* puNoData )
{
    if( nVarType == NC_DOUBLE )
        GetNoDataValueForDouble( nVarId, puNoData );
    else if( nVarType == NC_FLOAT )
        GetNoDataValueForFloat( nVarId, puNoData );
}

/************************************************************************/
/*                             SetXYZVars()                             */
/************************************************************************/

void netCDFLayer::SetXYZVars(int nXVarId, int nYVarId, int nZVarId)
{
    m_nXVarID = nXVarId;
    m_nYVarID = nYVarId;
    m_nZVarID = nZVarId;

    nc_inq_vartype( m_poDS->GetCDFID(), m_nXVarID, &m_nXVarNCDFType );
    nc_inq_vartype( m_poDS->GetCDFID(), m_nYVarID, &m_nYVarNCDFType );
    if( (m_nXVarNCDFType != NC_FLOAT && m_nXVarNCDFType != NC_DOUBLE) ||
        (m_nYVarNCDFType != NC_FLOAT && m_nYVarNCDFType != NC_DOUBLE) )
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "X or Y variable of type X=%d,Y=%d not handled",
                 m_nXVarNCDFType, m_nYVarNCDFType);
        m_nXVarID = -1;
        m_nYVarID = -1;
    }
    if( m_nZVarID >= 0 )
    {
        nc_inq_vartype( m_poDS->GetCDFID(), m_nZVarID, &m_nZVarNCDFType );
        if( m_nZVarNCDFType != NC_FLOAT && m_nZVarNCDFType != NC_DOUBLE )
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                      "Z variable of type %d not handled", m_nZVarNCDFType);
            m_nZVarID = -1;
        }
    }

    if( m_nXVarID >= 0 )
        GetNoDataValue( m_nXVarID, m_nXVarNCDFType, &m_uXVarNoData);
    if( m_nYVarID >= 0 )
        GetNoDataValue( m_nYVarID, m_nYVarNCDFType, &m_uYVarNoData);
    if( m_nZVarID >= 0 )
        GetNoDataValue( m_nZVarID, m_nZVarNCDFType, &m_uZVarNoData);
}

/************************************************************************/
/*                       SetWKTGeometryField()                          */
/************************************************************************/

void netCDFLayer::SetWKTGeometryField(const char* pszWKTVarName)
{
    m_nWKTVarID = -1;
    nc_inq_varid( m_poDS->GetCDFID(), pszWKTVarName, &m_nWKTVarID);
    if( m_nWKTVarID < 0 )
        return;
    int nd;
    nc_inq_varndims( m_poDS->GetCDFID(), m_nWKTVarID, &nd );
    nc_inq_vartype( m_poDS->GetCDFID(), m_nWKTVarID, &m_nWKTNCDFType );
#ifdef NETCDF_HAS_NC4
    if( nd == 1 && m_nWKTNCDFType == NC_STRING )
    {
        int nDimID;
        if( nc_inq_vardimid( m_poDS->GetCDFID(), m_nWKTVarID, &nDimID ) != NC_NOERR ||
            nDimID != m_nRecordDimID )
        {
            m_nWKTVarID = -1;
            return;
        }
    }
    else
#endif
    if (nd == 2 && m_nWKTNCDFType == NC_CHAR )
    {
        int anDimIds [] = { -1, -1 };
        size_t nLen = 0;
        if( nc_inq_vardimid( m_poDS->GetCDFID(), m_nWKTVarID, anDimIds ) != NC_NOERR ||
            anDimIds[0] != m_nRecordDimID ||
            nc_inq_dimlen( m_poDS->GetCDFID(), anDimIds[1], &nLen ) != NC_NOERR )
        {
            m_nWKTVarID = -1;
            return;
        }
        m_nWKTMaxWidth = static_cast<int>(nLen);
        m_nWKTMaxWidthDimId = anDimIds[1];
    }
    else
    {
        m_nWKTVarID = -1;
        return;
    }

    m_osWKTVarName = pszWKTVarName;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void netCDFLayer::ResetReading()
{
    m_nCurFeatureId = 1;
}


/************************************************************************/
/*                           Get1DVarAsDouble()                         */
/************************************************************************/

double netCDFLayer::Get1DVarAsDouble( int nVarId, nc_type nVarType,
                                      size_t nIndex,
                                      NCDFNoDataUnion noDataVal,
                                      bool* pbIsNoData )
{
    double dfVal = 0;
    if( nVarType == NC_DOUBLE )
    {
        nc_get_var1_double( m_poDS->GetCDFID(), nVarId, &nIndex, &dfVal );
        if( pbIsNoData)
            *pbIsNoData = ( dfVal == noDataVal.dfVal );
    }
    else if( nVarType == NC_FLOAT )
    {
        float fVal = 0.f;
        nc_get_var1_float( m_poDS->GetCDFID(), nVarId, &nIndex, &fVal );
        if( pbIsNoData)
            *pbIsNoData = ( fVal == noDataVal.fVal );
        dfVal = fVal;
    }
    else if( pbIsNoData)
        *pbIsNoData = true;
    return dfVal;
}

/************************************************************************/
/*                        GetNextRawFeature()                           */
/************************************************************************/

OGRFeature* netCDFLayer::GetNextRawFeature()
{
    m_poDS->SetDefineMode(false);

    size_t anIndex[2];
    anIndex[0] = m_nCurFeatureId-1;
    anIndex[1] = 0;

    OGRFeature* poFeature = new OGRFeature(m_poFeatureDefn);
    poFeature->SetFID(m_nCurFeatureId);
    m_nCurFeatureId ++;

    for(int i=0;i<m_poFeatureDefn->GetFieldCount();i++)
    {
        switch( m_anNCDFType[i] )
        {
            case NC_CHAR:
            {
                size_t anCount[2];
                anCount[0] = 1;
                anCount[1] = m_poFeatureDefn->GetFieldDefn(i)->GetWidth();
                if( anCount[1] == 0 )
                    anCount[1] = m_nDefaultMaxWidth;
                char* pszVal = (char*) CPLCalloc( 1, anCount[1] + 1 );
                int status = nc_get_vara_text( m_poDS->GetCDFID(),
                                    m_anVarId[i],
                                    anIndex, anCount,
                                    pszVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    CPLFree(pszVal);
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    CPLFree(pszVal);
                    continue;
                }
                poFeature->SetField(i, pszVal);
                CPLFree(pszVal);
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_STRING:
            {
                char* pszVal = NULL;
                int status = nc_get_var1_string( m_poDS->GetCDFID(),
                                    m_anVarId[i],
                                    anIndex,
                                    &pszVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( pszVal != NULL )
                {
                    poFeature->SetField(i, pszVal);
                    nc_free_string(1, &pszVal);
                }
                break;
            }
#endif

            case NC_BYTE:
            {
                signed char chVal = 0;
                int status = nc_get_var1_schar( m_poDS->GetCDFID(), m_anVarId[i],
                                                anIndex, &chVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( chVal == m_aNoData[i].chVal )
                    continue;
                poFeature->SetField(i, static_cast<int>(chVal));
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_UBYTE:
            {
                unsigned char uchVal = 0;
                int status = nc_get_var1_uchar( m_poDS->GetCDFID(), m_anVarId[i],
                                                anIndex, &uchVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( uchVal == m_aNoData[i].uchVal )
                    continue;
                poFeature->SetField(i, static_cast<int>(uchVal));
                break;
            }
#endif

            case NC_SHORT:
            {
                short sVal = 0;
                int status = nc_get_var1_short( m_poDS->GetCDFID(), m_anVarId[i],
                                    anIndex, &sVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( sVal == m_aNoData[i].sVal )
                    continue;
                poFeature->SetField(i, static_cast<int>(sVal));
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_USHORT:
            {
                unsigned short usVal = 0;
                int status = nc_get_var1_ushort( m_poDS->GetCDFID(), m_anVarId[i],
                                                 anIndex, &usVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( usVal == m_aNoData[i].usVal )
                    continue;
                poFeature->SetField(i, static_cast<int>(usVal));
                break;
            }
#endif

            case NC_INT:
            {
                int nVal = 0;
                int status = nc_get_var1_int( m_poDS->GetCDFID(), m_anVarId[i],
                                    anIndex, &nVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( nVal == m_aNoData[i].nVal )
                    continue;
                poFeature->SetField(i, nVal);
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_UINT:
            {
                unsigned int unVal = 0;
                // nc_get_var1_uint() doesn't work on old netCDF version when
                // the returned value is > INT_MAX
                // https://bugtracking.unidata.ucar.edu/browse/NCF-226
                // nc_get_vara_uint() has not this bug
                size_t nCount = 1;
                int status = nc_get_vara_uint( m_poDS->GetCDFID(), m_anVarId[i],
                                               anIndex, &nCount, &unVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( unVal == m_aNoData[i].unVal )
                    continue;
                poFeature->SetField(i, static_cast<GIntBig>(unVal));
                break;
            }
#endif

#ifdef NETCDF_HAS_NC4
            case NC_INT64:
            {
                GIntBig nVal = 0;
                int status = nc_get_var1_longlong( m_poDS->GetCDFID(), m_anVarId[i],
                                        anIndex, &nVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( nVal == m_aNoData[i].nVal64 )
                    continue;
                poFeature->SetField(i, nVal);
                break;
            }
#endif

            case NC_FLOAT:
            {
                float fVal = 0.f;
                int status = nc_get_var1_float( m_poDS->GetCDFID(), m_anVarId[i],
                                    anIndex, &fVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( fVal == m_aNoData[i].fVal )
                    continue;
                poFeature->SetField(i, static_cast<double>(fVal));
                break;
            }

            case NC_DOUBLE:
            {
                double dfVal = 0.0;
                int status = nc_get_var1_double( m_poDS->GetCDFID(), m_anVarId[i],
                                    anIndex, &dfVal );
                if( status == NC_EINVALCOORDS || status == NC_EEDGE )
                {
                    delete poFeature;
                    return NULL;
                }
                if( status != NC_NOERR )
                {
                    NCDF_ERR(status);
                    continue;
                }
                if( dfVal == m_aNoData[i].dfVal )
                    continue;
                if( m_poFeatureDefn->GetFieldDefn(i)->GetType() == OFTDate ||
                    m_poFeatureDefn->GetFieldDefn(i)->GetType() == OFTDateTime )
                {
                    struct tm brokendowntime;
                    GIntBig nVal = static_cast<GIntBig>(floor(dfVal));
                    CPLUnixTimeToYMDHMS( nVal, &brokendowntime );
                    poFeature->SetField( i,
                                        brokendowntime.tm_year + 1900,
                                        brokendowntime.tm_mon + 1,
                                        brokendowntime.tm_mday,
                                        brokendowntime.tm_hour,
                                        brokendowntime.tm_min,
                                        static_cast<float>(brokendowntime.tm_sec + (dfVal - nVal)),
                                     0 );
                }
                else
                {
                    poFeature->SetField(i, dfVal);
                }
                break;
            }

            default:
                break;
        }
    }

    if( m_nXVarID >= 0 && m_nYVarID >= 0 )
    {
        bool bXIsNoData = false;
        const double dfX = Get1DVarAsDouble( m_nXVarID, m_nXVarNCDFType,
                                             anIndex[0], m_uXVarNoData,
                                             &bXIsNoData );
        bool bYIsNoData = false;
        const double dfY = Get1DVarAsDouble( m_nYVarID, m_nYVarNCDFType,
                                             anIndex[0], m_uYVarNoData,
                                             &bYIsNoData );

        if( !bXIsNoData && !bYIsNoData ) 
        {
            OGRPoint* poPoint;
            if( m_nYVarID >= 0 )
            {
                bool bZIsNoData = false;
                const double dfZ = Get1DVarAsDouble( m_nZVarID, m_nZVarNCDFType,
                                                    anIndex[0], m_uZVarNoData,
                                                    &bZIsNoData );
                if( bZIsNoData )
                    poPoint = new OGRPoint(dfX, dfY);
                else
                    poPoint = new OGRPoint(dfX, dfY, dfZ);
            }
            else
                poPoint = new OGRPoint(dfX, dfY);
            poPoint->assignSpatialReference( GetSpatialRef() );
            poFeature->SetGeometryDirectly(poPoint);
        }
    }
    else if( m_nWKTVarID >= 0 )
    {
        char* pszWKT = NULL;
        if( m_nWKTNCDFType == NC_CHAR )
        {
            size_t anCount[2];
            anCount[0] = 1;
            anCount[1] = m_nWKTMaxWidth;
            pszWKT = (char*) CPLCalloc( 1, anCount[1] + 1 );
            int status = nc_get_vara_text( m_poDS->GetCDFID(), m_nWKTVarID,
                                           anIndex, anCount, pszWKT );
            if( status == NC_EINVALCOORDS || status == NC_EEDGE )
            {
                CPLFree(pszWKT);
                delete poFeature;
                return NULL;
            }
            if( status != NC_NOERR )
            {
                NCDF_ERR(status);
                CPLFree(pszWKT);
                pszWKT = NULL;
            }
        }
#ifdef NETCDF_HAS_NC4
        else if( m_nWKTNCDFType == NC_STRING )
        {
            char* pszVal = NULL;
            int status = nc_get_var1_string( m_poDS->GetCDFID(), m_nWKTVarID,
                                             anIndex, &pszVal );
            if( status == NC_EINVALCOORDS || status == NC_EEDGE )
            {
                delete poFeature;
                return NULL;
            }
            if( status != NC_NOERR )
            {
                NCDF_ERR(status);
            }
            else if( pszVal != NULL )
            {
                pszWKT = CPLStrdup(pszVal);
                nc_free_string(1, &pszVal);
            }
        }
#endif
        if( pszWKT != NULL )
        {
            char* pszWKTTmp = pszWKT;
            OGRGeometry* poGeom = NULL;
            CPL_IGNORE_RET_VAL( OGRGeometryFactory::createFromWkt( &pszWKTTmp, NULL, &poGeom ) );
            if( poGeom != NULL )
            {
                poGeom->assignSpatialReference( GetSpatialRef() );
                poFeature->SetGeometryDirectly(poGeom);
            }
            CPLFree(pszWKT);
        }
    }

    return poFeature;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature* netCDFLayer::GetNextFeature()
{
    while( true )
    {
        OGRFeature      *poFeature;

        poFeature = GetNextRawFeature();
        if( poFeature == NULL )
            return NULL;

        if( (m_poFilterGeom == NULL
            || FilterGeometry( poFeature->GetGeomFieldRef(m_iGeomFieldFilter) ) )
            && (m_poAttrQuery == NULL
                || m_poAttrQuery->Evaluate( poFeature )) )
            return poFeature;

        delete poFeature;
    }
}

/************************************************************************/
/*                            GetLayerDefn()                            */
/************************************************************************/

OGRFeatureDefn* netCDFLayer::GetLayerDefn()
{
    return m_poFeatureDefn;
}

/************************************************************************/
/*                           ICreateFeature()                           */
/************************************************************************/

OGRErr netCDFLayer::ICreateFeature(OGRFeature* poFeature)
{
    int status;
    m_poDS->SetDefineMode(false);

    size_t anIndex[2];
    anIndex[0] = m_nCurFeatureId-1;
    anIndex[1] = 0;

    for(int i=0;i<m_poFeatureDefn->GetFieldCount();i++)
    {
        if( !(poFeature->IsFieldSet(i)) )
            continue;

        status = NC_NOERR;
        switch( m_anNCDFType[i] )
        {
            case NC_CHAR:
            {
                const char* pszVal = poFeature->GetFieldAsString(i);
                size_t anCount[2];
                anCount[0] = 1;
                anCount[1] = strlen(pszVal);
                unsigned int nWidth = static_cast<unsigned int>
                    (m_poFeatureDefn->GetFieldDefn(i)->GetWidth());
                if( nWidth > 0 && anCount[1] > nWidth )
                {
                    anCount[1] = nWidth;
                }
                else if( nWidth == 0 && anCount[1] >
                            static_cast<unsigned int>(m_nDefaultMaxWidth) )
                {
                    anCount[1] = m_nDefaultMaxWidth;
                }
                status = nc_put_vara_text( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, anCount, pszVal );
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_STRING:
            {
                const char* pszVal = poFeature->GetFieldAsString(i);
                status = nc_put_var1_string( m_poDS->GetCDFID(), m_anVarId[i],
                                             anIndex, &pszVal );
                break;
            }
#endif

            case NC_BYTE:
            {
                int nVal = poFeature->GetFieldAsInteger(i);
                signed char chVal = static_cast<signed char>(nVal);
                status = nc_put_var1_schar( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &chVal );
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_UBYTE:
            {
                int nVal = poFeature->GetFieldAsInteger(i);
                unsigned char uchVal = static_cast<unsigned char>(nVal);
                status = nc_put_var1_uchar( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &uchVal );
                break;
            }
#endif

            case NC_SHORT:
            {
                int nVal = poFeature->GetFieldAsInteger(i);
                short sVal = static_cast<short>(nVal);
                status = nc_put_var1_short( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &sVal );
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_USHORT:
            {
                int nVal = poFeature->GetFieldAsInteger(i);
                unsigned short usVal = static_cast<unsigned short>(nVal);
                status = nc_put_var1_ushort( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &usVal );
                break;
            }
#endif

            case NC_INT:
            {
                int nVal = poFeature->GetFieldAsInteger(i);
                status = nc_put_var1_int( m_poDS->GetCDFID(), m_anVarId[i],
                                          anIndex, &nVal );
                break;
            }

#ifdef NETCDF_HAS_NC4
            case NC_UINT:
            {
                GIntBig nVal = poFeature->GetFieldAsInteger64(i);
                unsigned int unVal = static_cast<unsigned int>(nVal);
                status = nc_put_var1_uint( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &unVal );
                break;
            }
#endif

#ifdef NETCDF_HAS_NC4
            case NC_INT64:
            {
                GIntBig nVal = poFeature->GetFieldAsInteger64(i);
                status = nc_put_var1_longlong( m_poDS->GetCDFID(), m_anVarId[i],
                                               anIndex, &nVal );
                break;
            }
#endif

            case NC_FLOAT:
            {
                double dfVal = poFeature->GetFieldAsDouble(i);
                float fVal = static_cast<float>(dfVal);
                status = nc_put_var1_float( m_poDS->GetCDFID(), m_anVarId[i],
                                            anIndex, &fVal );
                break;
            }

            case NC_DOUBLE:
            {
                double dfVal;
                if( m_poFeatureDefn->GetFieldDefn(i)->GetType() == OFTDate ||
                    m_poFeatureDefn->GetFieldDefn(i)->GetType() == OFTDateTime )
                {
                    int nYear;
                    int nMonth;
                    int nDay;
                    int nHour;
                    int nMinute;
                    float fSecond;
                    int nTZ;
                    poFeature->GetFieldAsDateTime( i, &nYear, &nMonth, &nDay,
                                                &nHour, &nMinute, &fSecond, &nTZ );
                    struct tm brokendowntime;
                    brokendowntime.tm_year = nYear - 1900;
                    brokendowntime.tm_mon= nMonth - 1;
                    brokendowntime.tm_mday = nDay;
                    brokendowntime.tm_hour = nHour;
                    brokendowntime.tm_min = nMinute;
                    brokendowntime.tm_sec = static_cast<int>(fSecond);
                    GIntBig nVal = CPLYMDHMSToUnixTime(&brokendowntime);
                    dfVal = static_cast<double>(nVal) + fmod(fSecond, 1.0f);
                }
                else
                {
                    dfVal = poFeature->GetFieldAsDouble(i);
                }
                status = nc_put_var1_double( m_poDS->GetCDFID(), m_anVarId[i],
                                             anIndex, &dfVal );
                break;
            }

            default:
                break;
        }

        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return OGRERR_FAILURE;
        }
    }

    OGRGeometry* poGeom = poFeature->GetGeometryRef();
    if( wkbFlatten(m_poFeatureDefn->GetGeomType()) == wkbPoint &&
        poGeom != NULL &&
        wkbFlatten(poGeom->getGeometryType()) == wkbPoint )
    {
        double dfX = static_cast<OGRPoint*>(poGeom)->getX();
        double dfY = static_cast<OGRPoint*>(poGeom)->getY();

        status = nc_put_var1_double( m_poDS->GetCDFID(),
                                            m_nXVarID,
                                            anIndex,
                                            &dfX );
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return OGRERR_FAILURE;
        }

        status = nc_put_var1_double( m_poDS->GetCDFID(),
                                     m_nYVarID,
                                     anIndex,
                                     &dfY );
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return OGRERR_FAILURE;
        }

        if( m_poFeatureDefn->GetGeomType() == wkbPoint25D )
        {
            double dfZ = static_cast<OGRPoint*>(poGeom)->getZ();
            status = nc_put_var1_double( m_poDS->GetCDFID(),
                                         m_nZVarID,
                                         anIndex,
                                         &dfZ );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }
        }
    }
    else if( m_poFeatureDefn->GetGeomType() != wkbNone &&
             poGeom != NULL )
    {
        char* pszWKT = NULL;
        poGeom->exportToWkt( &pszWKT, wkbVariantIso );
#ifdef NETCDF_HAS_NC4
        if( m_nWKTNCDFType == NC_STRING )
        {
            const char* pszWKTConst = pszWKT;
            status = nc_put_var1_string( m_poDS->GetCDFID(), m_nWKTVarID,
                                         anIndex, &pszWKTConst );
        }
        else
#endif
        {
            size_t anCount[2];
            anCount[0] = 1;
            anCount[1] = strlen(pszWKT);
            if( anCount[1] > static_cast<unsigned int>(m_nWKTMaxWidth) )
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot write geometry as WKT. Would require %d "
                         "characters but field width is %d",
                         static_cast<int>(anCount[1]),
                         m_nWKTMaxWidth);
                status = NC_NOERR;
            }
            else
            {
                status = nc_put_vara_text( m_poDS->GetCDFID(), m_nWKTVarID,
                                           anIndex, anCount, pszWKT );
            }
        }
        CPLFree(pszWKT);
        NCDF_ERR(status);
        if ( status != NC_NOERR ) {
            return OGRERR_FAILURE;
        }
    }

    m_nCurFeatureId ++;
    poFeature->SetFID(m_nCurFeatureId);

    return OGRERR_NONE;
}

/************************************************************************/
/*                              AddField()                              */
/************************************************************************/

bool netCDFLayer::AddField(int nVarID)
{
    if( nVarID == m_nWKTVarID )
        return false;

    char szName[NC_MAX_NAME+1];
    szName[0] = '\0';
    CPL_IGNORE_RET_VAL(nc_inq_varname( m_poDS->GetCDFID(), nVarID, szName ));

    nc_type vartype=NC_NAT;
    nc_inq_vartype( m_poDS->GetCDFID(), nVarID, &vartype );

    OGRFieldType eType = OFTString;
    OGRFieldSubType eSubType = OFSTNone;
    int nWidth = 0;

    NCDFNoDataUnion nodata;
    memset(&nodata, 0, sizeof(nodata));

    switch( vartype )
    {
        case NC_BYTE:
        {
            eType = OFTInteger;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.chVal = static_cast<signed char>(atoi(pszValue));
            else
                nodata.chVal = NC_FILL_BYTE;
            CPLFree(pszValue);
            break;
        }

#ifdef NETCDF_HAS_NC4
        case NC_UBYTE:
        {
            eType = OFTInteger;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.uchVal = static_cast<unsigned char>(atoi(pszValue));
            else
                nodata.uchVal = NC_FILL_UBYTE;
            CPLFree(pszValue);
            break;
        }
#endif

        case NC_CHAR:
        {
            eType = OFTString;
            int nd;
            nc_inq_varndims( m_poDS->GetCDFID(), nVarID, &nd );
            if( nd == 1 )
            {
                nWidth = 1;
            }
            else if( nd == 2 )
            {
                int anDimIds[2] = { -1, -1 };
                nc_inq_vardimid( m_poDS->GetCDFID(), nVarID, anDimIds );
                size_t nDimLen = 0;
                nc_inq_dimlen( m_poDS->GetCDFID(), anDimIds[1], &nDimLen );
                nWidth = static_cast<int>(nDimLen);
            }
            break;
        }

#ifdef NETCDF_HAS_NC4
        case NC_STRING:
        {
            eType = OFTString;
            break;
        }
#endif

        case NC_SHORT:
        {
            eType = OFTInteger;
            eSubType = OFSTInt16;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.sVal = static_cast<short>(atoi(pszValue));
            else
                nodata.sVal = NC_FILL_SHORT;
            CPLFree(pszValue);
            break;
        }

#ifdef NETCDF_HAS_NC4
        case NC_USHORT:
        {
            eType = OFTInteger;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.usVal = static_cast<unsigned short>(atoi(pszValue));
            else
                nodata.usVal = NC_FILL_USHORT;
            CPLFree(pszValue);
            break;
        }
#endif

        case NC_INT:
        {
            eType = OFTInteger;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.nVal = atoi(pszValue);
            else
                nodata.nVal = NC_FILL_INT;
            CPLFree(pszValue);
            break;
        }

#ifdef NETCDF_HAS_NC4
        case NC_UINT:
        {
            eType = OFTInteger64;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.unVal = static_cast<unsigned int>(CPLAtoGIntBig(pszValue));
            else
                nodata.unVal = NC_FILL_UINT;
            CPLFree(pszValue);
            break;
        }
#endif

#ifdef NETCDF_HAS_NC4
        case NC_INT64:
        {
            eType = OFTInteger64;
            char* pszValue = NULL;
            if( GetFillValue( nVarID, &pszValue) == CE_None )
                nodata.nVal64 = CPLAtoGIntBig(pszValue);
            else
                nodata.nVal64 = NC_FILL_INT64;
            CPLFree(pszValue);
            break;
        }
#endif

        case NC_FLOAT:
        {
            eType = OFTReal;
            eSubType = OFSTFloat32;
            double dfValue;
            if( GetFillValue( nVarID, &dfValue) == CE_None )
                nodata.fVal = static_cast<float>(dfValue);
            else
                nodata.fVal = NC_FILL_FLOAT;
            break;
        }

        case NC_DOUBLE:
        {
            eType = OFTReal;
            double dfValue;
            if( GetFillValue( nVarID, &dfValue) == CE_None )
                nodata.dfVal = dfValue;
            else
                nodata.dfVal = NC_FILL_DOUBLE;
            break;
        }

        default:
        {
            CPLDebug("GDAL_netCDF", "Variable %s has type %d, which is unhandled",
                     szName, vartype);
            return false;
        }
    }

    char* pszValue = NULL;
    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarID, "ogr_field_type", &pszValue ) == CE_None )
    {
        if( (eType == OFTReal || eType == OFTDateTime) && EQUAL(pszValue, "Date") )
            eType = OFTDate;
        else if( eType == OFTReal && EQUAL(pszValue, "DateTime") )
            eType = OFTDateTime;
        else if( eType == OFTReal && EQUAL(pszValue, "Integer64") )
            eType = OFTInteger64;
        else if( eType == OFTInteger && EQUAL(pszValue, "Integer(Boolean)") )
            eSubType = OFSTBoolean;
    }
    CPLFree(pszValue);
    pszValue = NULL;

    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarID, "units", &pszValue ) == CE_None )
    {
        if( eType == OFTReal && EQUAL(pszValue, "seconds since 1970-1-1 0:0:0") )
            eType = OFTDateTime;
    }
    CPLFree(pszValue);
    pszValue = NULL;

    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarID, "ogr_field_name", &pszValue ) == CE_None )
    {
        snprintf(szName, sizeof(szName), "%s", pszValue);
    }
    CPLFree(pszValue);
    pszValue = NULL;

    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarID, "ogr_field_width", &pszValue ) == CE_None )
    {
        nWidth = atoi(pszValue);
    }
    CPLFree(pszValue);
    pszValue = NULL;

    int nPrecision = 0;
    if( NCDFGetAttr( m_poDS->GetCDFID(), nVarID, "ogr_field_precision", &pszValue ) == CE_None )
    {
        nPrecision = atoi(pszValue);
    }
    CPLFree(pszValue);
    /* pszValue = NULL; */

    OGRFieldDefn oFieldDefn(szName, eType);
    oFieldDefn.SetSubType(eSubType);
    oFieldDefn.SetWidth(nWidth);
    oFieldDefn.SetPrecision(nPrecision);

    m_anVarId.push_back(nVarID);
    m_aNoData.push_back(nodata);
    m_anNCDFType.push_back(vartype);

    m_poFeatureDefn->AddFieldDefn(&oFieldDefn);

    return true;
}

/************************************************************************/
/*                             CreateField()                            */
/************************************************************************/

OGRErr netCDFLayer::CreateField(OGRFieldDefn* poFieldDefn, int /* bApproxOK */)
{
    int nSecDimID = -1;
    int nVarID = -1;
    int status;

    // Try to use the field name as variable name, but detects conflict first
    CPLString osVarName;
    osVarName = poFieldDefn->GetNameRef();
    status = nc_inq_varid( m_poDS->GetCDFID(), osVarName, &nVarID );
    if( status == NC_NOERR )
    {
        for(int i=1;i<=100;i++)
        {
            osVarName = CPLSPrintf("%s%d", poFieldDefn->GetNameRef(), i);
            status = nc_inq_varid( m_poDS->GetCDFID(), osVarName, &nVarID );
            if( status != NC_NOERR )
                break;
        }
        CPLDebug("netCDF", "Field %s is written in variable %s",
                 poFieldDefn->GetNameRef(), osVarName.c_str());
    }

    const char* pszVarName = osVarName.c_str();

    NCDFNoDataUnion nodata;
    memset(&nodata, 0, sizeof(nodata));

    const OGRFieldType eType = poFieldDefn->GetType();
    const OGRFieldSubType eSubType = poFieldDefn->GetSubType();
    nc_type nType = NC_NAT;
    switch( eType )
    {
        case OFTString:
        case OFTStringList:
        case OFTIntegerList:
        case OFTRealList:
        {
#ifdef NETCDF_HAS_NC4
            if( m_poDS->eFormat == NCDF_FORMAT_NC4 && m_bUseStringInNC4 )
            {
                nType = NC_STRING;
                status = nc_def_var( m_poDS->GetCDFID(),
                                     pszVarName,
                                     nType, 1, &m_nRecordDimID, &nVarID );
            }
            else
#endif
            {
                if( poFieldDefn->GetWidth() == 0 )
                {
                    if( m_nDefaultMaxWidthDimId < 0 )
                    {
                        status = nc_def_dim( m_poDS->GetCDFID(),
                                            "string_default_max_width",
                                            m_nDefaultMaxWidth,
                                            &m_nDefaultMaxWidthDimId );
                        NCDF_ERR(status);
                        if ( status != NC_NOERR ) {
                            return OGRERR_FAILURE;
                        }
                    }
                    nSecDimID = m_nDefaultMaxWidthDimId;
                }
                else
                {
                    status = nc_def_dim( m_poDS->GetCDFID(),
                                        CPLSPrintf("%s_max_width", pszVarName),
                                        poFieldDefn->GetWidth(), &nSecDimID );
                    NCDF_ERR(status);
                    if ( status != NC_NOERR ) {
                        return OGRERR_FAILURE;
                    }
                }

                int anDims[] = { m_nRecordDimID, nSecDimID };
                nType = NC_CHAR;
                status = nc_def_var( m_poDS->GetCDFID(),
                                    pszVarName,
                                    nType, 2, anDims, &nVarID );
            }
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }

            break;
        }

        case OFTInteger:
        {
            nType = (eSubType == OFSTBoolean) ? NC_BYTE :
                    (eSubType == OFSTInt16) ?   NC_SHORT :
                                                NC_INT;

            if( nType == NC_BYTE )
                nodata.chVal = NC_FILL_BYTE;
            else if( nType == NC_SHORT )
                nodata.sVal = NC_FILL_SHORT;
            else if( nType == NC_INT )
                nodata.nVal = NC_FILL_INT;

            status = nc_def_var( m_poDS->GetCDFID(),
                                 pszVarName,
                                 nType, 1, &m_nRecordDimID, &nVarID );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }

            if( eSubType == OFSTBoolean )
            {
                signed char anRange[] = { 0, 1 };
                nc_put_att_schar( m_poDS->GetCDFID(),nVarID, "valid_range",
                                  NC_BYTE, 2, anRange );
            }

            break;
        }

        case OFTInteger64:
        {
            nType = NC_DOUBLE;
            nodata.dfVal = NC_FILL_DOUBLE;
#ifdef NETCDF_HAS_NC4
            if( m_poDS->eFormat == NCDF_FORMAT_NC4 )
            {
                nType = NC_INT64;
                nodata.nVal64 = NC_FILL_INT64;
            }
#endif
            status = nc_def_var( m_poDS->GetCDFID(),
                                 pszVarName,
                                 nType, 1, &m_nRecordDimID, &nVarID );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }
            break;
        }

        case OFTReal:
        {
            nType = ( eSubType == OFSTFloat32 ) ? NC_FLOAT : NC_DOUBLE;
            if( eSubType == OFSTFloat32 )
                nodata.fVal = NC_FILL_FLOAT;
            else
                nodata.dfVal = NC_FILL_DOUBLE;
            status = nc_def_var( m_poDS->GetCDFID(),
                                 pszVarName,
                                 nType, 1, &m_nRecordDimID, &nVarID );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }

            break;
        }

        case OFTDate:
        case OFTDateTime:
        {
            nType = NC_DOUBLE;
            status = nc_def_var( m_poDS->GetCDFID(),
                                 pszVarName,
                                 nType, 1, &m_nRecordDimID, &nVarID );
            NCDF_ERR(status);
            if ( status != NC_NOERR ) {
                return OGRERR_FAILURE;
            }
            nodata.dfVal = NC_FILL_DOUBLE;

            status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, CF_UNITS, 
                             strlen("seconds since 1970-1-1 0:0:0"),
                             "seconds since 1970-1-1 0:0:0" );
            NCDF_ERR(status);

            break;
        }

        default:
            return OGRERR_FAILURE;
    }

    m_anVarId.push_back(nVarID);
    m_aNoData.push_back(nodata);
    m_anNCDFType.push_back(nType);

    const char* pszLongName = CPLSPrintf("Field %s", poFieldDefn->GetNameRef());
    status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, CF_LNG_NAME,
                     strlen(pszLongName), pszLongName);
    NCDF_ERR(status);

    if( m_bWriteGDALTags )
    {
        status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, "ogr_field_name",
                        strlen(poFieldDefn->GetNameRef()), poFieldDefn->GetNameRef());
        NCDF_ERR(status);

        const char* pszType = OGRFieldDefn::GetFieldTypeName(eType);
        if( eSubType != OFSTNone )
        {
            pszType = CPLSPrintf("%s(%s)", pszType,
                                 OGRFieldDefn::GetFieldSubTypeName(eSubType));
        }
        status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, "ogr_field_type",
                         strlen(pszType), pszType );
        NCDF_ERR(status);

        const int nWidth = poFieldDefn->GetWidth();
        if( nWidth )
        {
            status = nc_put_att_int( m_poDS->GetCDFID(), nVarID, "ogr_field_width",
                            NC_INT, 1, &nWidth );
            NCDF_ERR(status);

            const int nPrecision = poFieldDefn->GetPrecision();
            if( nPrecision )
            {
                status = nc_put_att_int( m_poDS->GetCDFID(), nVarID, "ogr_field_precision",
                                NC_INT, 1, &nPrecision );
                NCDF_ERR(status);
            }
        }
    }

    //nc_put_att_text( m_poDS->GetCDFID(), nVarID, CF_UNITS,
    //                 strlen("none"), "none");

    if( m_pszCFProjection != NULL )
    {
        status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, CF_GRD_MAPPING, 
                         strlen(m_pszCFProjection), m_pszCFProjection );
        NCDF_ERR(status);
    }

    if( m_osCoordinatesValue.size() )
    {
        status = nc_put_att_text( m_poDS->GetCDFID(), nVarID, CF_COORDINATES, 
                        m_osCoordinatesValue.size(), m_osCoordinatesValue.c_str() );
        NCDF_ERR(status);
    }

    m_poFeatureDefn->AddFieldDefn(poFieldDefn);
    return OGRERR_NONE;
}

/************************************************************************/
/*                         GetFeatureCount()                            */
/************************************************************************/

GIntBig netCDFLayer::GetFeatureCount(int bForce)
{
    if( m_poFilterGeom == NULL && m_poAttrQuery == NULL )
    {
        size_t nDimLen;
        nc_inq_dimlen ( m_poDS->GetCDFID(), m_nRecordDimID, &nDimLen );
        return static_cast<GIntBig>(nDimLen);
    }
    return OGRLayer::GetFeatureCount(bForce);
}

/************************************************************************/
/*                          TestCapability()                            */
/************************************************************************/

int netCDFLayer::TestCapability(const char* pszCap)
{
    if( EQUAL(pszCap, OLCSequentialWrite) )
        return m_poDS->GetAccess() == GA_Update;
    if( EQUAL(pszCap, OLCCreateField) )
        return m_poDS->GetAccess() == GA_Update;
    if( EQUAL(pszCap, OLCFastFeatureCount) )
        return( m_poFilterGeom == NULL && m_poAttrQuery == NULL );
    return FALSE;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int netCDFDataset::Identify( GDALOpenInfo * poOpenInfo )

{
    if( STARTS_WITH_CI(poOpenInfo->pszFilename, "NETCDF:") ) {
        return TRUE;
    }
    const NetCDFFormatEnum nTmpFormat = IdentifyFormat( poOpenInfo );
    if( NCDF_FORMAT_NC == nTmpFormat ||
        NCDF_FORMAT_NC2 == nTmpFormat ||
        NCDF_FORMAT_NC4 == nTmpFormat ||
        NCDF_FORMAT_NC4C == nTmpFormat )
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *netCDFDataset::Open( GDALOpenInfo * poOpenInfo )

{
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "\n=====\nOpen(), filename=[%s]", poOpenInfo->pszFilename );
#endif

/* -------------------------------------------------------------------- */
/*      Does this appear to be a netcdf file?                           */
/* -------------------------------------------------------------------- */
    NetCDFFormatEnum eTmpFormat = NCDF_FORMAT_NONE;
    if( ! STARTS_WITH_CI(poOpenInfo->pszFilename, "NETCDF:") ) {
        eTmpFormat = IdentifyFormat( poOpenInfo );
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "identified format %d", eTmpFormat );
#endif
        /* Note: not calling Identify() directly, because we want the file type */
        /* Only support NCDF_FORMAT* formats */
        if( ! ( NCDF_FORMAT_NC  == eTmpFormat ||
                NCDF_FORMAT_NC2  == eTmpFormat ||
                NCDF_FORMAT_NC4  == eTmpFormat ||
                NCDF_FORMAT_NC4C  == eTmpFormat ) )
            return NULL;
    }

    CPLMutexHolderD(&hNCMutex);

    CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
    netCDFDataset *poDS = new netCDFDataset();
    CPLAcquireMutex(hNCMutex, 1000.0);

    poDS->SetDescription( poOpenInfo->pszFilename );

/* -------------------------------------------------------------------- */
/*       Check if filename start with NETCDF: tag                       */
/* -------------------------------------------------------------------- */
    bool bTreatAsSubdataset = false;
    CPLString osSubdatasetName;

    if( STARTS_WITH_CI(poOpenInfo->pszFilename, "NETCDF:") )
    {
        char **papszName =
            CSLTokenizeString2( poOpenInfo->pszFilename,
                                ":", CSLT_HONOURSTRINGS|CSLT_PRESERVEESCAPES );

        /* -------------------------------------------------------------------- */
        /*    Check for drive name in windows NETCDF:"D:\...                    */
        /* -------------------------------------------------------------------- */
        if ( CSLCount(papszName) == 4 &&
             strlen(papszName[1]) == 1 &&
             (papszName[2][0] == '/' || papszName[2][0] == '\\') )
        {
            poDS->osFilename = papszName[1];
            poDS->osFilename += ':';
            poDS->osFilename += papszName[2];
            osSubdatasetName = papszName[3];
            bTreatAsSubdataset = true;
            CSLDestroy( papszName );
        }
        else if( CSLCount(papszName) == 3 )
        {
            poDS->osFilename = papszName[1];
            osSubdatasetName = papszName[2];
            bTreatAsSubdataset = true;
            CSLDestroy( papszName );
    	}
        else if( CSLCount(papszName) == 2 )
        {
            poDS->osFilename = papszName[1];
            osSubdatasetName = "";
            bTreatAsSubdataset = false;
            CSLDestroy( papszName );
    	}
        else
        {
            CSLDestroy( papszName );
            CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
            delete poDS;
            CPLAcquireMutex(hNCMutex, 1000.0);
            CPLError( CE_Failure, CPLE_AppDefined,
                      "Failed to parse NETCDF: prefix string into expected 2, 3 or 4 fields." );
            return NULL;
        }
        /* Identify Format from real file, with bCheckExt=FALSE */ 
        GDALOpenInfo* poOpenInfo2 = new GDALOpenInfo(poDS->osFilename.c_str(), GA_ReadOnly );
        poDS->eFormat = IdentifyFormat( poOpenInfo2, FALSE );
        delete poOpenInfo2;
        if( NCDF_FORMAT_NONE == poDS->eFormat ||
            NCDF_FORMAT_UNKNOWN == poDS->eFormat ) {
            CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
            delete poDS;
            CPLAcquireMutex(hNCMutex, 1000.0);
            return NULL;
        }
    }
    else
    {
        poDS->osFilename = poOpenInfo->pszFilename;
        bTreatAsSubdataset = false;
        poDS->eFormat = eTmpFormat;
    }

/* -------------------------------------------------------------------- */
/*      Try opening the dataset.                                        */
/* -------------------------------------------------------------------- */
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "calling nc_open( %s )", poDS->osFilename.c_str() );
#endif
    int cdfid;
    if( nc_open( poDS->osFilename, NC_NOWRITE, &cdfid ) != NC_NOERR ) {
#ifdef NCDF_DEBUG
        CPLDebug( "GDAL_netCDF", "error opening" );
#endif
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }
#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "got cdfid=%d\n", cdfid );
#endif

/* -------------------------------------------------------------------- */
/*      Is this a real netCDF file?                                     */
/* -------------------------------------------------------------------- */
    int ndims;
    int ngatts;
    int nvars;
    int unlimdimid;
    int status = nc_inq(cdfid, &ndims, &nvars, &ngatts, &unlimdimid);
    if( status != NC_NOERR ) {
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Get file type from netcdf                                       */
/* -------------------------------------------------------------------- */
    int nTmpFormat = NCDF_FORMAT_NONE;
    status = nc_inq_format (cdfid, &nTmpFormat);
    if ( status != NC_NOERR ) {
        NCDF_ERR(status);
    }
    else {
        CPLDebug( "GDAL_netCDF", 
                  "driver detected file type=%d, libnetcdf detected type=%d",
                  poDS->eFormat, nTmpFormat );
        if ( static_cast<NetCDFFormatEnum>(nTmpFormat) != poDS->eFormat ) {
            /* warn if file detection conflicts with that from libnetcdf */
            /* except for NC4C, which we have no way of detecting initially */
            if ( nTmpFormat != NCDF_FORMAT_NC4C ) {
                CPLError( CE_Warning, CPLE_AppDefined, 
                          "NetCDF driver detected file type=%d, but libnetcdf detected type=%d",
                          poDS->eFormat, nTmpFormat );
            }
            CPLDebug( "GDAL_netCDF", "setting file type to %d, was %d", 
                      nTmpFormat, poDS->eFormat );
            poDS->eFormat = static_cast<NetCDFFormatEnum>(nTmpFormat);
        }
    }

/* -------------------------------------------------------------------- */
/*      Confirm the requested access is supported.                      */
/* -------------------------------------------------------------------- */
    if( poOpenInfo->eAccess == GA_Update )
    {
        CPLError( CE_Failure, CPLE_NotSupported, 
                  "The NETCDF driver does not support update access to existing"
                  " datasets.\n" );
        nc_close( cdfid );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Does the request variable exist?                                */
/* -------------------------------------------------------------------- */
    if( bTreatAsSubdataset )
    {
        int var;
        status = nc_inq_varid( cdfid, osSubdatasetName, &var);
        if( status != NC_NOERR ) {
            CPLError( CE_Warning, CPLE_AppDefined, 
                      "%s is a netCDF file, but %s is not a variable.",
                      poOpenInfo->pszFilename, 
                      osSubdatasetName.c_str() );

            nc_close( cdfid );
            CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
            delete poDS;
            CPLAcquireMutex(hNCMutex, 1000.0);
            return NULL;
        }
    }

    if( ndims < 2 && (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) == 0 )
    {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "%s is a netCDF file, but without any dimensions >= 2.",
                  poOpenInfo->pszFilename );

        nc_close( cdfid );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

    CPLDebug( "GDAL_netCDF", "dim_count = %d", ndims );

    char szConventions[NC_MAX_NAME+1];
    szConventions[0] = '\0';
    nc_type nAttype=NC_NAT;
    size_t nAttlen = 0;
    nc_inq_att( cdfid, NC_GLOBAL, "Conventions", &nAttype, &nAttlen);
    if( nAttlen >= sizeof(szConventions) ||
        (status = nc_get_att_text( cdfid, NC_GLOBAL, "Conventions",
                                   szConventions )) != NC_NOERR ) {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "No UNIDATA NC_GLOBAL:Conventions attribute");
        /* note that 'Conventions' is always capital 'C' in CF spec*/
    }
    else
    {
        szConventions[nAttlen] = '\0';
    }


/* -------------------------------------------------------------------- */
/*      Create band information objects.                                */
/* -------------------------------------------------------------------- */
    CPLDebug( "GDAL_netCDF", "var_count = %d", nvars );

/* -------------------------------------------------------------------- */
/*      Create a corresponding GDALDataset.                             */
/*      Create Netcdf Subdataset if filename as NETCDF tag              */
/* -------------------------------------------------------------------- */
    poDS->cdfid = cdfid;

    poDS->ReadAttributes( cdfid, NC_GLOBAL );	

/* -------------------------------------------------------------------- */
/*  Identify variables that we should ignore as Raster Bands.           */
/*  Variables that are identified in other variable's "coordinate" and  */
/*  "bounds" attribute should not be treated as Raster Bands.           */
/*  See CF sections 5.2, 5.6 and 7.1                                    */
/* -------------------------------------------------------------------- */
    char **papszIgnoreVars = NULL;
    char *pszTemp = NULL;

    for ( int j = 0; j < nvars; j++ ) {
        char **papszTokens = NULL;
        if ( NCDFGetAttr( cdfid, j, "coordinates", &pszTemp ) == CE_None ) { 
            papszTokens = CSLTokenizeString2( pszTemp, " ", 0 );
            for ( int i=0; i<CSLCount(papszTokens); i++ ) {
                papszIgnoreVars = CSLAddString( papszIgnoreVars, papszTokens[i] );
            }
            if ( papszTokens) CSLDestroy( papszTokens );
            CPLFree( pszTemp );
        }
        if ( NCDFGetAttr( cdfid, j, "bounds", &pszTemp ) == CE_None &&
             pszTemp != NULL ) { 
            if ( !EQUAL( pszTemp, "" ) )
                papszIgnoreVars = CSLAddString( papszIgnoreVars, pszTemp );
            CPLFree( pszTemp );
        }
    }

/* -------------------------------------------------------------------- */
/*  Filter variables (valid 2D raster bands and vector fields)          */
/* -------------------------------------------------------------------- */
    int nCount = 0;
    int nIgnoredVars = 0;
    int nVarID = -1;
    std::vector<int> anPotentialVectorVarID;
    // oMapDimIdToCount[x] = number of times dim x is the first dimension of potential vector variables
    std::map<int, int> oMapDimIdToCount;
    int nVarXId = -1;
    int nVarYId = -1;
    int nVarZId = -1;

    for ( int j = 0; j < nvars; j++ ) {
        int ndimsForVar = -1;
        char szTemp[NC_MAX_NAME+1];
        nc_inq_varndims ( cdfid, j, &ndimsForVar );
        /* should we ignore this variable ? */
        szTemp[0] = '\0';
        status = nc_inq_varname( cdfid, j, szTemp );
        if ( status != NC_NOERR )
            continue;

        if( ndimsForVar == 1 &&
            (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 &&
            (NCDFIsVarLongitude( cdfid, -1, szTemp) ||
             NCDFIsVarProjectionX( cdfid, -1, szTemp)) )
        {
            nVarXId = j;
        }
        else if( ndimsForVar == 1 &&
            (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 &&
            (NCDFIsVarLatitude( cdfid, -1, szTemp) ||
             NCDFIsVarProjectionY( cdfid, -1, szTemp)) )
        {
            nVarYId = j;
        }
        else if( ndimsForVar == 1 &&
            (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 &&
            NCDFIsVarVerticalCoord( cdfid, -1, szTemp) )
        {
            nVarZId = j;
        }
        else if ( CSLFindString( papszIgnoreVars, szTemp ) != -1 ) {
            nIgnoredVars++;
            CPLDebug( "GDAL_netCDF", "variable #%d [%s] was ignored",j, szTemp);
        }
        /* only accept 2+D vars */
        else if( ndimsForVar >= 2 ) {

            // Identify variables that might be vector variables
            if( ndimsForVar == 2 && (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 )
            {
                int anDimIds[2] = { -1, -1 };
                nc_inq_vardimid( cdfid, j, anDimIds );
                char szDimNameX[NC_MAX_NAME+1], szDimNameY[NC_MAX_NAME+1];
                szDimNameX[0]='\0';
                szDimNameY[0]='\0';
                if ( nc_inq_dimname( cdfid, anDimIds[0], szDimNameY ) == NC_NOERR &&
                     nc_inq_dimname( cdfid, anDimIds[1], szDimNameX ) == NC_NOERR &&
                     NCDFIsVarLongitude( cdfid, -1, szDimNameX )==false && 
                     NCDFIsVarProjectionX( cdfid, -1, szDimNameX )==false &&
                     NCDFIsVarLatitude( cdfid, -1, szDimNameY )==false &&
                     NCDFIsVarProjectionY( cdfid, -1, szDimNameY )==false )
                {
                    anPotentialVectorVarID.push_back(j);
                    oMapDimIdToCount[ anDimIds[0] ] ++;
                }
            }

            if( (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) != 0 )
            {
                nVarID=j;
                nCount++;
            }
        }
        else if( ndimsForVar == 1 &&
                 (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 )
        {
            anPotentialVectorVarID.push_back(j);
            int nDimId = -1;
            nc_inq_vardimid( cdfid, j, &nDimId );
            oMapDimIdToCount[ nDimId ] ++;
        }
    }

    CSLDestroy( papszIgnoreVars );

    if( anPotentialVectorVarID.size() )
    {
        // Take the dimension that is referenced the most times
        int nVectorDim = oMapDimIdToCount.rbegin()->first;
        if( oMapDimIdToCount.size() != 1 )
        {
            char szVarName[NC_MAX_NAME+1];
            szVarName[0] = '\0';
            CPL_IGNORE_RET_VAL(nc_inq_varname( cdfid, nVectorDim, szVarName ));
            CPLError(CE_Warning, CPLE_AppDefined,
                     "The dataset has several variables that could be identified "
                     "as vector fields, but not all share the same primary dimension. "
                     "Consequently they will be ignored.");
        }
        else
        {
            OGRwkbGeometryType eGType = wkbUnknown;
            CPLString osLayerName = CSLFetchNameValueDef(
                poDS->papszMetadata, "NC_GLOBAL#ogr_layer_name",
                CPLGetBasename(poDS->osFilename));
            poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#ogr_layer_name", NULL);

            if( EQUAL(CSLFetchNameValueDef(poDS->papszMetadata, "NC_GLOBAL#featureType", ""), "point") )
            {
                poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#featureType", NULL);
                eGType = wkbPoint;
            }

            const char* pszLayerType = CSLFetchNameValue(poDS->papszMetadata, "NC_GLOBAL#ogr_layer_type");
            if( pszLayerType != NULL )
            {
                eGType = OGRFromOGCGeomType(pszLayerType);
                poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#ogr_layer_type", NULL);
            }

            CPLString osGeometryField = CSLFetchNameValueDef(poDS->papszMetadata, "NC_GLOBAL#ogr_geometry_field", "");
            poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#ogr_geometry_field", NULL);

            int nFirstVarId = -1;
            for( size_t j = 0; j < anPotentialVectorVarID.size(); j++ )
            {
                int anDimIds[2] = { -1, -1 };
                nc_inq_vardimid( cdfid, anPotentialVectorVarID[j], anDimIds );
                if( nVectorDim == anDimIds[0] )
                {
                    nFirstVarId = anPotentialVectorVarID[j];
                    break;
                }
            }

            // In case where coordinates are explicitly specified for one of the field/variable,
            // use them in priority over the ones that might have been identified above
            char* pszCoordinates = NULL;
            if( NCDFGetAttr( cdfid, nFirstVarId, "coordinates", &pszCoordinates) == CE_None )
            {
                char** papszTokens = CSLTokenizeString2( pszCoordinates, " ", 0 );
                for ( int i=0; papszTokens != NULL && papszTokens[i] != NULL; i++ )
                {
                    if( NCDFIsVarLongitude( cdfid, -1, papszTokens[i] ) ||
                        NCDFIsVarProjectionX( cdfid, -1, papszTokens[i] ) )
                    {
                        nVarXId = -1;
                        CPL_IGNORE_RET_VAL(nc_inq_varid(cdfid, papszTokens[i], &nVarXId));
                    }
                    else if( NCDFIsVarLatitude( cdfid, -1, papszTokens[i] ) ||
                             NCDFIsVarProjectionY( cdfid, -1, papszTokens[i] ) )
                    {
                        nVarYId = -1;
                        CPL_IGNORE_RET_VAL(nc_inq_varid(cdfid, papszTokens[i], &nVarYId));
                    }
                    else if( NCDFIsVarVerticalCoord( cdfid, -1, papszTokens[i] ) )
                    {
                        nVarZId = -1;
                        CPL_IGNORE_RET_VAL(nc_inq_varid(cdfid, papszTokens[i], &nVarZId));
                    }
                }
                CSLDestroy(papszTokens);
            }
            CPLFree(pszCoordinates);

            if( eGType == wkbUnknown && nVarXId >= 0 && nVarYId >= 0 )
            {
                eGType = wkbPoint;
            }
            if( eGType == wkbPoint && nVarZId >= 0 )
            {
                eGType = wkbPoint25D;
            }
            if( eGType == wkbUnknown && osGeometryField.size() == 0 )
            {
                eGType = wkbNone;
            }

            // Read projection info
            char** papszMetadataBackup = CSLDuplicate(poDS->papszMetadata);
            poDS->ReadAttributes( cdfid, nFirstVarId );
            poDS->SetProjectionFromVar( nFirstVarId, true );
            CSLDestroy(poDS->papszMetadata);
            poDS->papszMetadata = papszMetadataBackup;

            OGRSpatialReference* poSRS = NULL;
            if( poDS->pszProjection != NULL )
            {
                poSRS = new OGRSpatialReference();
                char* pszWKT = poDS->pszProjection;
                if( poSRS->importFromWkt(&pszWKT) != OGRERR_NONE )
                {
                    delete poSRS;
                    poSRS = NULL;
                }
                CPLFree(poDS->pszProjection);
                poDS->pszProjection = NULL;
            }
            // Reset if there's a 2D raster
            poDS->bSetProjection = false;
            poDS->bSetGeoTransform = false;

            if( (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) == 0 )
            {
                // Strip out uninteresting metadata
                poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#Conventions", NULL);
                poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#GDAL", NULL);
                poDS->papszMetadata = CSLSetNameValue(poDS->papszMetadata, "NC_GLOBAL#history", NULL);
            }

            netCDFLayer* poLayer = new netCDFLayer(poDS, osLayerName, eGType, poSRS);
            if( poSRS != NULL )
                poSRS->Release();
            poLayer->SetRecordDimID(nVectorDim);
            if( wkbFlatten(eGType) == wkbPoint && nVarXId >= 0 && nVarYId >= 0 )
            {
                poLayer->SetXYZVars( nVarXId, nVarYId, nVarZId );
            }
            else if( osGeometryField.size() )
            {
                poLayer->SetWKTGeometryField( osGeometryField );
            }
            poDS->papoLayers = static_cast<OGRLayer**>(
                CPLRealloc(poDS->papoLayers, (poDS->nLayers + 1) * sizeof(OGRLayer)));
            poDS->papoLayers[poDS->nLayers++] = poLayer;

            for( size_t j = 0; j < anPotentialVectorVarID.size(); j++ )
            {
                int anDimIds[2] = { -1, -1 };
                nc_inq_vardimid( cdfid, anPotentialVectorVarID[j], anDimIds );
                if( nVectorDim == anDimIds[0] )
                {
#ifdef NCDF_DEBUG
                    char szTemp[NC_MAX_NAME+1];
                    szTemp[0] = '\0';
                    CPL_IGNORE_RET_VAL(nc_inq_varname( cdfid, anPotentialVectorVarID[j], szTemp ));
                    CPLDebug("GDAL_netCDF", "Variable %s is a vector field", szTemp);
#endif
                    poLayer->AddField( anPotentialVectorVarID[j] );
                }
            }
        }
    }

    // Case where there is no raster variable
    if( nCount == 0 && !bTreatAsSubdataset )
    {
        poDS->SetMetadata( poDS->papszMetadata );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        poDS->TryLoadXML();
        // If the dataset has been opened in raster mode only, exit
        if( (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) != 0 &&
            (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) == 0 )
        {
            delete poDS;
            poDS = NULL;
        }
        // Otherwise if the dataset is opened in vector mode, that there is
        // no vector layer and we are in read-only, exit too.
        else if( poDS->nLayers == 0 &&
                 (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR) != 0 &&
                 poOpenInfo->eAccess == GA_ReadOnly )
        {
            delete poDS;
            poDS = NULL;
        }
        CPLAcquireMutex(hNCMutex, 1000.0);
        return( poDS );
    }

/* -------------------------------------------------------------------- */
/*      We have more than one variable with 2 dimensions in the         */
/*      file, then treat this as a subdataset container dataset.        */
/* -------------------------------------------------------------------- */
    if( (nCount > 1) && !bTreatAsSubdataset )
    {
        poDS->CreateSubDatasetList();
        poDS->SetMetadata( poDS->papszMetadata );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        poDS->TryLoadXML();
        CPLAcquireMutex(hNCMutex, 1000.0);
        return( poDS );
    }

/* -------------------------------------------------------------------- */
/*      If we are not treating things as a subdataset, then capture     */
/*      the name of the single available variable as the subdataset.    */
/* -------------------------------------------------------------------- */
    if( !bTreatAsSubdataset )
    {
        char szVarName[NC_MAX_NAME+1];
        szVarName[0] = '\0';
        status = nc_inq_varname( cdfid, nVarID, szVarName);
        NCDF_ERR(status);
        osSubdatasetName = szVarName;
    }

/* -------------------------------------------------------------------- */
/*      We have ignored at least one variable, so we should report them */
/*      as subdatasets for reference.                                   */
/* -------------------------------------------------------------------- */
    if( (nIgnoredVars > 0) && !bTreatAsSubdataset )
    {
        CPLDebug( "GDAL_netCDF", 
                  "As %d variables were ignored, creating subdataset list "
                  "for reference. Variable #%d [%s] is the main variable", 
                  nIgnoredVars, nVarID, osSubdatasetName.c_str() );
        poDS->CreateSubDatasetList();
    }

/* -------------------------------------------------------------------- */
/*      Open the NETCDF subdataset NETCDF:"filename":subdataset         */
/* -------------------------------------------------------------------- */
    int var=-1;
    nc_inq_varid( cdfid, osSubdatasetName, &var);
    int nd = 0;
    nc_inq_varndims ( cdfid, var, &nd );

    int *paDimIds  = reinterpret_cast<int *>( CPLCalloc(nd, sizeof( int ) ) );

    // X, Y, Z position in array
    int *panBandDimPos
        = reinterpret_cast<int *>( CPLCalloc( nd, sizeof( int ) ) );

    nc_inq_vardimid( cdfid, var, paDimIds );

/* -------------------------------------------------------------------- */
/*      Check if somebody tried to pass a variable with less than 2D    */
/* -------------------------------------------------------------------- */
    if ( nd < 2 ) {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "Variable has %d dimension(s) - not supported.", nd );
        CPLFree( paDimIds );
        CPLFree( panBandDimPos );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      CF-1 Convention                                                 */
/*      dimensions to appear in the relative order T, then Z, then Y,   */
/*      then X  to the file. All other dimensions should, whenever      */
/*      possible, be placed to the left of the spatiotemporal           */
/*      dimensions.                                                     */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/*      Verify that dimensions are in the {T,Z,Y,X} or {T,Z,Y,X} order  */
/*      Ideally we should detect for other ordering and act accordingly */
/*      Only done if file has Conventions=CF-* and only prints warning  */
/*      To disable set GDAL_NETCDF_VERIFY_DIMS=NO and to use only       */
/*      attributes (not varnames) set GDAL_NETCDF_VERIFY_DIMS=STRICT    */
/* -------------------------------------------------------------------- */

    bool bCheckDims =
        CPLTestBool( CPLGetConfigOption( "GDAL_NETCDF_VERIFY_DIMS", "YES" ) )
        && STARTS_WITH_CI(szConventions, "CF");

    char szDimName[NC_MAX_NAME+1];

    if ( bCheckDims ) {
        char szDimName1[NC_MAX_NAME+1], szDimName2[NC_MAX_NAME+1], 
            szDimName3[NC_MAX_NAME+1], szDimName4[NC_MAX_NAME+1];
        szDimName1[0]='\0';
        szDimName2[0]='\0';
        szDimName3[0]='\0';
        szDimName4[0]='\0';
        status = nc_inq_dimname( cdfid, paDimIds[nd-1], szDimName1 );
        NCDF_ERR(status);
        status = nc_inq_dimname( cdfid, paDimIds[nd-2], szDimName2 );
        NCDF_ERR(status);
        if (  NCDFIsVarLongitude( cdfid, -1, szDimName1 )==false && 
              NCDFIsVarProjectionX( cdfid, -1, szDimName1 )==false ) {
            CPLError( CE_Warning, CPLE_AppDefined, 
                      "dimension #%d (%s) is not a Longitude/X dimension.", 
                      nd-1, szDimName1 );
        }
        if ( NCDFIsVarLatitude( cdfid, -1, szDimName2 )==false &&
             NCDFIsVarProjectionY( cdfid, -1, szDimName2 )==false ) {
            CPLError( CE_Warning, CPLE_AppDefined, 
                      "dimension #%d (%s) is not a Latitude/Y dimension.", 
                      nd-2, szDimName2 );
        }
        if ( nd >= 3 ) {
            status = nc_inq_dimname( cdfid, paDimIds[nd-3], szDimName3 );
            NCDF_ERR(status);
            if ( nd >= 4 ) {
                status = nc_inq_dimname( cdfid, paDimIds[nd-4], szDimName4 );
                NCDF_ERR(status);
                if ( NCDFIsVarVerticalCoord( cdfid, -1, szDimName3 )==false ) {
                    CPLError( CE_Warning, CPLE_AppDefined, 
                              "dimension #%d (%s) is not a Time  dimension.", 
                              nd-3, szDimName3 );
                }
                if ( NCDFIsVarTimeCoord( cdfid, -1, szDimName4 )==false ) {
                    CPLError( CE_Warning, CPLE_AppDefined, 
                              "dimension #%d (%s) is not a Time  dimension.", 
                              nd-4, szDimName4 );
                }
            }
            else {
                if ( NCDFIsVarVerticalCoord( cdfid, -1, szDimName3 )==false && 
                     NCDFIsVarTimeCoord( cdfid, -1, szDimName3 )==false ) {
                    CPLError( CE_Warning, CPLE_AppDefined, 
                              "dimension #%d (%s) is not a Time or Vertical dimension.", 
                              nd-3, szDimName3 );
                }
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Get X dimensions information                                    */
/* -------------------------------------------------------------------- */
    size_t xdim;
    poDS->nXDimID = paDimIds[nd-1];
    nc_inq_dimlen ( cdfid, poDS->nXDimID, &xdim );

/* -------------------------------------------------------------------- */
/*      Get Y dimension information                                     */
/* -------------------------------------------------------------------- */
    size_t ydim;
    poDS->nYDimID = paDimIds[nd-2];
    nc_inq_dimlen ( cdfid, poDS->nYDimID, &ydim );

    if( xdim > INT_MAX || ydim > INT_MAX )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid raster dimensions: " CPL_FRMT_GUIB "x" CPL_FRMT_GUIB,
                 static_cast<GUIntBig>(xdim),
                 static_cast<GUIntBig>(ydim));
        CPLFree( paDimIds );
        CPLFree( panBandDimPos );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

    poDS->nRasterXSize = static_cast<int>(xdim);
    poDS->nRasterYSize = static_cast<int>(ydim);

    unsigned int k = 0;
    for( int j=0; j < nd; j++ ){
        if( paDimIds[j] == poDS->nXDimID ){ 
            panBandDimPos[0] = j;         // Save Position of XDim
            k++;
        }
        if( paDimIds[j] == poDS->nYDimID ){
            panBandDimPos[1] = j;         // Save Position of YDim
            k++;
        }
    }
/* -------------------------------------------------------------------- */
/*      X and Y Dimension Ids were not found!                           */
/* -------------------------------------------------------------------- */
    if( k != 2 ) {
        CPLFree( paDimIds );
        CPLFree( panBandDimPos );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Read Metadata for this variable                                 */
/* -------------------------------------------------------------------- */
/* should disable as is also done at band level, except driver needs the 
   variables as metadata (e.g. projection) */
    poDS->ReadAttributes( cdfid, var );

/* -------------------------------------------------------------------- */
/*      Read Metadata for each dimension                                */
/* -------------------------------------------------------------------- */
    for( int j=0; j < ndims; j++ ){
        char szTemp[NC_MAX_NAME+1];
        status = nc_inq_dimname( cdfid, j, szTemp );
        NCDF_ERR(status);
        poDS->papszDimName.AddString( szTemp );
        int nDimID;
        status = nc_inq_varid( cdfid, poDS->papszDimName[j], &nDimID );
        if( status == NC_NOERR ) {
            poDS->ReadAttributes( cdfid, nDimID );
        }
    }

/* -------------------------------------------------------------------- */
/*      Set projection info                                             */
/* -------------------------------------------------------------------- */
    poDS->SetProjectionFromVar( var, false );

    /* override bottom-up with GDAL_NETCDF_BOTTOMUP config option */
    const char *pszValue = CPLGetConfigOption( "GDAL_NETCDF_BOTTOMUP", NULL );
    if ( pszValue ) {
        poDS->bBottomUp = CPLTestBool( pszValue );
        CPLDebug( "GDAL_netCDF", 
                  "set bBottomUp=%d because GDAL_NETCDF_BOTTOMUP=%s",
                  static_cast<int>(poDS->bBottomUp), pszValue );
    }

/* -------------------------------------------------------------------- */
/*      Save non-spatial dimension info                                 */
/* -------------------------------------------------------------------- */

    int *panBandZLev = NULL;
    int nDim = 2;
    size_t lev_count;
    size_t nTotLevCount = 1;
    nc_type nType = NC_NAT;

    CPLString osExtraDimNames;

    if ( nd > 2 ) {
        nDim=2;
        panBandZLev = (int *)CPLCalloc( nd-2, sizeof( int ) );

        osExtraDimNames = "{";

        for( int j=0; j < nd; j++ ){
            if( ( paDimIds[j] != poDS->nXDimID ) && 
                ( paDimIds[j] != poDS->nYDimID ) ){
                nc_inq_dimlen ( cdfid, paDimIds[j], &lev_count );
                nTotLevCount *= lev_count;
                panBandZLev[ nDim-2 ] = static_cast<int>(lev_count);
                panBandDimPos[ nDim++ ] = j; //Save Position of ZDim
                //Save non-spatial dimension names
                if ( nc_inq_dimname( cdfid, paDimIds[j], szDimName ) 
                     == NC_NOERR ) {
                    osExtraDimNames += szDimName;
                    if ( j < nd-3 ) {
                        osExtraDimNames += ",";
                    }
                    nc_inq_varid( cdfid, szDimName, &nVarID );
                    nc_inq_vartype( cdfid, nVarID, &nType );
                    char szExtraDimDef[NC_MAX_NAME+1];
                    snprintf( szExtraDimDef, sizeof(szExtraDimDef), "{%ld,%d}", (long)lev_count, nType );
                    char szTemp[NC_MAX_NAME+32+1];
                    snprintf( szTemp, sizeof(szTemp), "NETCDF_DIM_%s_DEF", szDimName );
                    poDS->papszMetadata = CSLSetNameValue( poDS->papszMetadata, 
                                                           szTemp, szExtraDimDef );
                    if ( NCDFGet1DVar( cdfid, nVarID, &pszTemp ) == CE_None ) {
                        snprintf( szTemp, sizeof(szTemp), "NETCDF_DIM_%s_VALUES", szDimName );
                        poDS->papszMetadata = CSLSetNameValue( poDS->papszMetadata, 
                                                              szTemp, pszTemp );
                        CPLFree( pszTemp );
                    }
                }
            }
        }
        osExtraDimNames += "}";
        poDS->papszMetadata = CSLSetNameValue( poDS->papszMetadata, 
                                               "NETCDF_DIM_EXTRA",
                                               osExtraDimNames );
    }

/* -------------------------------------------------------------------- */
/*      Store Metadata                                                  */
/* -------------------------------------------------------------------- */
    poDS->SetMetadata( poDS->papszMetadata );

/* -------------------------------------------------------------------- */
/*      Create bands                                                    */
/* -------------------------------------------------------------------- */

    /* Arbitrary threshold */
    int nMaxBandCount = atoi(CPLGetConfigOption("GDAL_MAX_BAND_COUNT", "32768"));
    if( nMaxBandCount <= 0 )
        nMaxBandCount = 32768;
    if( nTotLevCount > static_cast<unsigned int>(nMaxBandCount) )
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Limiting number of bands to %d instead of %u",
                 nMaxBandCount,
                 static_cast<unsigned int>(nTotLevCount));
        nTotLevCount = static_cast<unsigned int>(nMaxBandCount);
    }
    for ( unsigned int lev = 0; lev < nTotLevCount ; lev++ ) {
        netCDFRasterBand *poBand =
            new netCDFRasterBand(poDS, var, nDim, lev,
                                 panBandZLev, panBandDimPos, 
                                 paDimIds, lev+1 );
        poDS->SetBand( lev+1, poBand );
    }

    CPLFree( paDimIds );
    CPLFree( panBandDimPos );
    if ( panBandZLev )
        CPLFree( panBandZLev );
    // Handle angular geographic coordinates here

/* -------------------------------------------------------------------- */
/*      Initialize any PAM information.                                 */
/* -------------------------------------------------------------------- */
    if( bTreatAsSubdataset )
    {
        poDS->SetPhysicalFilename( poDS->osFilename );
        poDS->SetSubdatasetName( osSubdatasetName );
    }

    CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
    poDS->TryLoadXML();

    if( bTreatAsSubdataset )
        poDS->oOvManager.Initialize( poDS, ":::VIRTUAL:::" );
    else
        poDS->oOvManager.Initialize( poDS, poDS->osFilename );

    CPLAcquireMutex(hNCMutex, 1000.0);

    return( poDS );
}

/************************************************************************/
/*                            CopyMetadata()                            */
/*                                                                      */
/*      Create a copy of metadata for NC_GLOBAL or a variable           */
/************************************************************************/

static void CopyMetadata( void  *poDS, int fpImage, int CDFVarID, 
                   const char *pszPrefix, bool bIsBand ) {

    char       **papszFieldData=NULL;

    /* Remove the following band meta but set them later from band data */
    const char *papszIgnoreBand[] = { CF_ADD_OFFSET, CF_SCALE_FACTOR, 
                                      "valid_range", "_Unsigned", 
                                      _FillValue, "coordinates", 
                                      NULL };
    const char *papszIgnoreGlobal[] = { "NETCDF_DIM_EXTRA", NULL };

    char **papszMetadata = NULL;
    if( CDFVarID == NC_GLOBAL ) {
        papszMetadata = GDALGetMetadata( (GDALDataset *) poDS,"");
    } else {
        papszMetadata = GDALGetMetadata( (GDALRasterBandH) poDS, NULL );
    }

    const int nItems = CSLCount( papszMetadata );

    for(int k=0; k < nItems; k++ ) {
        const char *pszField = CSLGetField( papszMetadata, k );
        if ( papszFieldData ) CSLDestroy( papszFieldData );
        papszFieldData = CSLTokenizeString2 (pszField, "=", 
                                             CSLT_HONOURSTRINGS );
        if( papszFieldData[1] != NULL ) {

#ifdef NCDF_DEBUG
            CPLDebug( "GDAL_netCDF", "copy metadata [%s]=[%s]", 
                      papszFieldData[ 0 ], papszFieldData[ 1 ] );
#endif

            CPLString osMetaName(papszFieldData[ 0 ]);
            CPLString osMetaValue(papszFieldData[ 1 ]);

            /* check for items that match pszPrefix if applicable */
            if ( ( pszPrefix != NULL ) && ( !EQUAL( pszPrefix, "" ) ) ) {
                    /* remove prefix */
                    if ( EQUALN( osMetaName, pszPrefix, strlen(pszPrefix) ) ) {
                        osMetaName = osMetaName.substr(strlen(pszPrefix));
                    }
                    /* only copy items that match prefix */
                    else
                        continue;
            }

            /* Fix various issues with metadata translation */ 
            if( CDFVarID == NC_GLOBAL ) {
                /* Do not copy items in papszIgnoreGlobal and NETCDF_DIM_* */
                if ( ( CSLFindString( (char **)papszIgnoreGlobal, osMetaName ) != -1 ) ||
                     ( STARTS_WITH(osMetaName, "NETCDF_DIM_") ) )
                    continue;
                /* Remove NC_GLOBAL prefix for netcdf global Metadata */ 
                else if( STARTS_WITH(osMetaName, "NC_GLOBAL#") ) {
                    osMetaName = osMetaName.substr(strlen("NC_GLOBAL#"));
                } 
                /* GDAL Metadata renamed as GDAL-[meta] */
                else if ( strstr( osMetaName, "#" ) == NULL ) {
                    osMetaName = "GDAL_" + osMetaName;
                }
                /* Keep time, lev and depth information for safe-keeping */
                /* Time and vertical coordinate handling need improvements */
                /*
                else if( STARTS_WITH(szMetaName, "time#") ) {
                    szMetaName[4] = '-';
                }
                else if( STARTS_WITH(szMetaName, "lev#") ) {
                    szMetaName[3] = '-';
                }
                else if( STARTS_WITH(szMetaName, "depth#") ) {
                    szMetaName[5] = '-';
                }
                */
                /* Only copy data without # (previously all data was copied)  */
                if ( strstr( osMetaName, "#" ) != NULL )
                    continue;
                // /* netCDF attributes do not like the '#' character. */
                // for( unsigned int h=0; h < strlen( szMetaName ) -1 ; h++ ) {
                //     if( szMetaName[h] == '#' ) szMetaName[h] = '-'; 
                // }
            }
            else {
                /* Do not copy varname, stats, NETCDF_DIM_*, nodata 
                   and items in papszIgnoreBand */
                if ( ( STARTS_WITH(osMetaName, "NETCDF_VARNAME") ) ||
                     ( STARTS_WITH(osMetaName, "STATISTICS_") ) ||
                     ( STARTS_WITH(osMetaName, "NETCDF_DIM_") ) ||
                     ( STARTS_WITH(osMetaName, "missing_value") ) ||
                     ( STARTS_WITH(osMetaName, "_FillValue") ) ||
                     ( CSLFindString( (char **)papszIgnoreBand, osMetaName ) != -1 ) )
                    continue;
            }

#ifdef NCDF_DEBUG
            CPLDebug( "GDAL_netCDF", "copy name=[%s] value=[%s]",
                      osMetaName.c_str(), osMetaValue.c_str() );
#endif
            if ( NCDFPutAttr( fpImage, CDFVarID,osMetaName, 
                              osMetaValue ) != CE_None )
                CPLDebug( "GDAL_netCDF", "NCDFPutAttr(%d, %d, %s, %s) failed", 
                          fpImage, CDFVarID,osMetaName.c_str(), osMetaValue.c_str() );
        }
    }

    if ( papszFieldData ) CSLDestroy( papszFieldData );

    /* Set add_offset and scale_factor here if present */
    if( ( CDFVarID != NC_GLOBAL ) && ( bIsBand ) ) {

        int bGotAddOffset, bGotScale;
        GDALRasterBandH poRB = (GDALRasterBandH) poDS;
        const double dfAddOffset = GDALGetRasterOffset( poRB , &bGotAddOffset );
        const double dfScale = GDALGetRasterScale( poRB, &bGotScale );

        if ( bGotAddOffset && dfAddOffset != 0.0 && bGotScale && dfScale != 1.0 ) {
            GDALSetRasterOffset( poRB, dfAddOffset );
            GDALSetRasterScale( poRB, dfScale );
        }
    }
}

/************************************************************************/
/*                            CreateLL()                                */
/*                                                                      */
/*      Shared functionality between netCDFDataset::Create() and        */
/*      netCDF::CreateCopy() for creating netcdf file based on a set of */
/*      options and a configuration.                                    */
/************************************************************************/

netCDFDataset *
netCDFDataset::CreateLL( const char * pszFilename,
                         int nXSize, int nYSize, int nBands,
                         char ** papszOptions )
{
    if( !((nXSize == 0 && nYSize == 0 && nBands == 0) ||
          (nXSize > 0 && nYSize > 0 && nBands > 0)) )
    {
        return NULL;
    }

    CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
    netCDFDataset *poDS = new netCDFDataset();
    CPLAcquireMutex(hNCMutex, 1000.0);

    poDS->nRasterXSize = nXSize;
    poDS->nRasterYSize = nYSize;
    poDS->eAccess = GA_Update;
    poDS->osFilename = pszFilename;

    /* from gtiff driver, is this ok? */
    /*
    poDS->nBlockXSize = nXSize;
    poDS->nBlockYSize = 1;
    poDS->nBlocksPerBand =
        ((nYSize + poDS->nBlockYSize - 1) / poDS->nBlockYSize)
        * ((nXSize + poDS->nBlockXSize - 1) / poDS->nBlockXSize);
        */

    /* process options */
    poDS->papszCreationOptions = CSLDuplicate( papszOptions );
    poDS->ProcessCreationOptions( );

/* -------------------------------------------------------------------- */
/*      Create the dataset.                                             */
/* -------------------------------------------------------------------- */
    int status = nc_create( pszFilename, poDS->nCreateMode,  &(poDS->cdfid) );

    /* put into define mode */
    poDS->SetDefineMode(true);

    if( status != NC_NOERR )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "Unable to create netCDF file %s (Error code %d): %s .\n", 
                  pszFilename, status, nc_strerror(status) );
        CPLReleaseMutex(hNCMutex); // Release mutex otherwise we'll deadlock with GDALDataset own mutex
        delete poDS;
        CPLAcquireMutex(hNCMutex, 1000.0);
        return NULL;
    }

/* -------------------------------------------------------------------- */
/*      Define dimensions                                               */
/* -------------------------------------------------------------------- */
    if( nXSize > 0 && nYSize > 0 )
    {
        poDS->papszDimName.AddString( NCDF_DIMNAME_X );
        status = nc_def_dim( poDS->cdfid, NCDF_DIMNAME_X, nXSize, 
                            &(poDS->nXDimID) );
        NCDF_ERR(status);
        CPLDebug( "GDAL_netCDF", "status nc_def_dim( %d, %s, %d, -) got id %d", 
                poDS->cdfid, NCDF_DIMNAME_X, nXSize, poDS->nXDimID );   

        poDS->papszDimName.AddString( NCDF_DIMNAME_Y );
        status = nc_def_dim( poDS->cdfid, NCDF_DIMNAME_Y, nYSize, 
                            &(poDS->nYDimID) );
        NCDF_ERR(status);
        CPLDebug( "GDAL_netCDF", "status nc_def_dim( %d, %s, %d, -) got id %d", 
                poDS->cdfid, NCDF_DIMNAME_Y, nYSize, poDS->nYDimID );   
    }

    return poDS;
}

/************************************************************************/
/*                            Create()                                  */
/************************************************************************/

GDALDataset *
netCDFDataset::Create( const char * pszFilename,
                       int nXSize, int nYSize, int nBands,
                       GDALDataType eType,
                       char ** papszOptions )
{
    CPLDebug( "GDAL_netCDF", 
              "\n=====\nnetCDFDataset::Create( %s, ... )\n", 
              pszFilename );

    CPLMutexHolderD(&hNCMutex);

    netCDFDataset *poDS =  netCDFDataset::CreateLL( pszFilename,
                                                    nXSize, nYSize, nBands,
                                                    papszOptions );
    if ( ! poDS )
        return NULL;

    /* should we write signed or unsigned byte? */
    /* TODO should this only be done in Create() */
    poDS->bSignedData = true;
    const char *pszValue  =
        CSLFetchNameValueDef( papszOptions, "PIXELTYPE", "" );
    if( eType == GDT_Byte && ( ! EQUAL(pszValue,"SIGNEDBYTE") ) )
        poDS->bSignedData = false;

/* -------------------------------------------------------------------- */
/*      Add Conventions, GDAL info and history                          */
/* -------------------------------------------------------------------- */
    NCDFAddGDALHistory( poDS->cdfid, pszFilename, "", "Create",
                        (nBands == 0) ? NCDF_CONVENTIONS_CF_V1_6 : NCDF_CONVENTIONS_CF_V1_5);

/* -------------------------------------------------------------------- */
/*      Define bands                                                    */
/* -------------------------------------------------------------------- */
    for( int iBand = 1; iBand <= nBands; iBand++ )
    {
        poDS->SetBand( iBand, new netCDFRasterBand( poDS, eType, iBand,
                                                    poDS->bSignedData ) );
    }

    CPLDebug( "GDAL_netCDF", 
              "netCDFDataset::Create( %s, ... ) done", 
              pszFilename );
/* -------------------------------------------------------------------- */
/*      Return same dataset                                             */
/* -------------------------------------------------------------------- */
     return( poDS );
}


template <class T>
static CPLErr  NCDFCopyBand( GDALRasterBand *poSrcBand, GDALRasterBand *poDstBand,
                      int nXSize, int nYSize,
                      GDALProgressFunc pfnProgress, void * pProgressData )
{
    GDALDataType eDT = poSrcBand->GetRasterDataType();
    CPLErr eErr = CE_None;
    T *patScanline = (T *) CPLMalloc( nXSize * sizeof(T) );

    for( int iLine = 0; iLine < nYSize && eErr == CE_None; iLine++ )  {
        eErr = poSrcBand->RasterIO( GF_Read, 0, iLine, nXSize, 1, 
                                    patScanline, nXSize, 1, eDT,
                                    0,0, NULL);
        if ( eErr != CE_None )
            CPLDebug( "GDAL_netCDF", 
                      "NCDFCopyBand(), poSrcBand->RasterIO() returned error code %d",
                      eErr );
        else { 
            eErr = poDstBand->RasterIO( GF_Write, 0, iLine, nXSize, 1, 
                                        patScanline, nXSize, 1, eDT,
                                        0,0, NULL);
            if ( eErr != CE_None )
                CPLDebug( "GDAL_netCDF", 
                          "NCDFCopyBand(), poDstBand->RasterIO() returned error code %d",
                          eErr );
        }

        if ( ( nYSize>10 ) && ( iLine % (nYSize/10) == 1 ) ) {
            if( !pfnProgress( 1.0*iLine/nYSize , NULL, pProgressData ) )
            {
                eErr = CE_Failure;
                CPLError( CE_Failure, CPLE_UserInterrupt,
                        "User terminated CreateCopy()" );
            }
        }
    }

    CPLFree( patScanline );

    pfnProgress( 1.0, NULL, pProgressData );

    return eErr;
}

/************************************************************************/
/*                            CreateCopy()                              */
/************************************************************************/

GDALDataset*
netCDFDataset::CreateCopy( const char * pszFilename, GDALDataset *poSrcDS,
                           CPL_UNUSED int bStrict, char ** papszOptions,
                           GDALProgressFunc pfnProgress, void * pProgressData )
{
    CPLMutexHolderD(&hNCMutex);

    CPLDebug( "GDAL_netCDF", 
              "\n=====\nnetCDFDataset::CreateCopy( %s, ... )\n", 
              pszFilename );

    const int nBands = poSrcDS->GetRasterCount();
    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();
    const char *pszWKT = poSrcDS->GetProjectionRef();

/* -------------------------------------------------------------------- */
/*      Check input bands for errors                                    */
/* -------------------------------------------------------------------- */

    if (nBands == 0)
    {
        CPLError( CE_Failure, CPLE_NotSupported, 
                  "NetCDF driver does not support source dataset with zero band.\n");
        return NULL;
    }

    GDALDataType eDT;
    GDALRasterBand *poSrcBand = NULL;
    for( int iBand=1; iBand <= nBands; iBand++ )
    {
        poSrcBand = poSrcDS->GetRasterBand( iBand );
        eDT = poSrcBand->GetRasterDataType();
        if (eDT == GDT_Unknown || GDALDataTypeIsComplex(eDT))
        {
            CPLError( CE_Failure, CPLE_NotSupported, 
                      "NetCDF driver does not support source dataset with band of complex type.");
            return NULL;
        }
    }

    if( !pfnProgress( 0.0, NULL, pProgressData ) )
        return NULL;

    /* same as in Create() */
    netCDFDataset *poDS = netCDFDataset::CreateLL( pszFilename,
                                                   nXSize, nYSize, nBands,
                                                   papszOptions );
    if ( ! poDS )
        return NULL;

/* -------------------------------------------------------------------- */
/*      Copy global metadata                                            */
/*      Add Conventions, GDAL info and history                          */
/* -------------------------------------------------------------------- */
    CopyMetadata((void *) poSrcDS, poDS->cdfid, NC_GLOBAL, NULL, false );
    NCDFAddGDALHistory( poDS->cdfid, pszFilename,
                        poSrcDS->GetMetadataItem("NC_GLOBAL#history",""),
                        "CreateCopy" );

    pfnProgress( 0.1, NULL, pProgressData );

/* -------------------------------------------------------------------- */
/*      Check for extra dimensions                                      */
/* -------------------------------------------------------------------- */
    int nDim = 2;
    char **papszExtraDimNames =
        NCDFTokenizeArray( poSrcDS->GetMetadataItem("NETCDF_DIM_EXTRA","") );
    char **papszExtraDimValues = NULL;

    if ( papszExtraDimNames != NULL && ( CSLCount( papszExtraDimNames )> 0 ) ) {
        size_t nDimSize = 0;
        size_t nDimSizeTot = 1;
        // first make sure dimensions lengths compatible with band count
        // for ( int i=0; i<CSLCount( papszExtraDimNames ); i++ ) {
        for ( int i=CSLCount( papszExtraDimNames )-1; i>=0; i-- ) {
            char szTemp[ NC_MAX_NAME + 32 + 1 ];
            snprintf( szTemp, sizeof(szTemp), "NETCDF_DIM_%s_DEF", papszExtraDimNames[i] );
            papszExtraDimValues = NCDFTokenizeArray( poSrcDS->GetMetadataItem(szTemp,"") );
            nDimSize = atol( papszExtraDimValues[0] );
            CSLDestroy( papszExtraDimValues );
            nDimSizeTot *= nDimSize;
        }
        if ( nDimSizeTot == (size_t)nBands ) {
            nDim = 2 + CSLCount( papszExtraDimNames );
        }
        else {
            // if nBands != #bands computed raise a warning
            // just issue a debug message, because it was probably intentional
            CPLDebug( "GDAL_netCDF",
                      "Warning: Number of bands (%d) is not compatible with dimensions "
                      "(total=%ld names=%s)", nBands, (long)nDimSizeTot,
                      poSrcDS->GetMetadataItem("NETCDF_DIM_EXTRA","") );
            CSLDestroy( papszExtraDimNames );
            papszExtraDimNames = NULL;
        }
    }

    int *panDimIds = reinterpret_cast<int *>( CPLCalloc( nDim, sizeof( int ) ) );
    int *panBandDimPos = reinterpret_cast<int *>( CPLCalloc( nDim, sizeof( int ) ) );

    nc_type nVarType;
    int status = NC_NOERR;
    int *panBandZLev = NULL;
    int *panDimVarIds = NULL;

    if ( nDim > 2 ) {
        panBandZLev = reinterpret_cast<int *>( CPLCalloc( nDim-2, sizeof( int ) ) );
        panDimVarIds = reinterpret_cast<int *>( CPLCalloc( nDim-2, sizeof( int ) ) );

        /* define all dims */
        for ( int i=CSLCount( papszExtraDimNames )-1; i>=0; i-- ) {
            poDS->papszDimName.AddString( papszExtraDimNames[i] );
            char szTemp[ NC_MAX_NAME + 32 + 1 ];
            snprintf( szTemp, sizeof(szTemp), "NETCDF_DIM_%s_DEF", papszExtraDimNames[i] );
            papszExtraDimValues = NCDFTokenizeArray( poSrcDS->GetMetadataItem(szTemp,"") );
            int nDimSize = atoi( papszExtraDimValues[0] );
            /* nc_type is an enum in netcdf-3, needs casting */
            nVarType = (nc_type) atol( papszExtraDimValues[1] );
            CSLDestroy( papszExtraDimValues );
            panBandZLev[ i ] = nDimSize;
            panBandDimPos[ i+2 ] = i; //Save Position of ZDim

            /* define dim */
            status = nc_def_dim( poDS->cdfid, papszExtraDimNames[i], nDimSize, 
                                 &(panDimIds[i]) );
            NCDF_ERR(status);

            /* define dim var */
            int anDim[1];
            anDim[0] = panDimIds[i];
            status = nc_def_var( poDS->cdfid, papszExtraDimNames[i],  
                                 nVarType, 1, anDim, 
                                 &(panDimVarIds[i]) );
            NCDF_ERR(status);

            /* add dim metadata, using global var# items */
            snprintf( szTemp, sizeof(szTemp), "%s#", papszExtraDimNames[i] );
            CopyMetadata((void *) poSrcDS, poDS->cdfid, panDimVarIds[i], szTemp, false );
        }
    }

/* -------------------------------------------------------------------- */
/*      Copy GeoTransform and Projection                                */
/* -------------------------------------------------------------------- */
    /* copy geolocation info */
    if ( poSrcDS->GetMetadata("GEOLOCATION") != NULL )
        poDS->SetMetadata( poSrcDS->GetMetadata("GEOLOCATION"), "GEOLOCATION" );

    /* copy geotransform */
    bool bGotGeoTransform = false;
    double adfGeoTransform[6];
    CPLErr eErr = poSrcDS->GetGeoTransform( adfGeoTransform );
    if ( eErr == CE_None ) {
        poDS->SetGeoTransform( adfGeoTransform );
        /* disable AddProjectionVars() from being called */
        bGotGeoTransform = true;
        poDS->bSetGeoTransform = false;
    }

    /* copy projection */
    void *pScaledProgress;
    if ( pszWKT ) {
        poDS->SetProjection( pszWKT );
        /* now we can call AddProjectionVars() directly */
        poDS->bSetGeoTransform = bGotGeoTransform;
        pScaledProgress = GDALCreateScaledProgress( 0.1, 0.25, pfnProgress, 
                                                    pProgressData );
        poDS->AddProjectionVars( GDALScaledProgress, pScaledProgress );
        /* save X,Y dim positions */
        panDimIds[nDim-1] = poDS->nXDimID;
        panBandDimPos[0] = nDim-1; 
        panDimIds[nDim-2] = poDS->nYDimID;
        panBandDimPos[1] = nDim-2; 
        GDALDestroyScaledProgress( pScaledProgress );

    }

    /* write extra dim values - after projection for optimization */
    if ( nDim > 2 ) { 
        /* make sure we are in data mode */
        reinterpret_cast<netCDFDataset *>( poDS )->SetDefineMode( false );
        for ( int i=CSLCount( papszExtraDimNames )-1; i>=0; i-- ) {
            char szTemp[ NC_MAX_NAME + 32 + 1 ];
            snprintf( szTemp, sizeof(szTemp), "NETCDF_DIM_%s_VALUES", papszExtraDimNames[i] );
            if ( poSrcDS->GetMetadataItem( szTemp ) != NULL ) {
                NCDFPut1DVar( poDS->cdfid, panDimVarIds[i], 
                              poSrcDS->GetMetadataItem( szTemp ) );
            }
        }
    }

    pfnProgress( 0.25, NULL, pProgressData );

/* -------------------------------------------------------------------- */
/*      Define Bands                                                    */
/* -------------------------------------------------------------------- */

    netCDFRasterBand *poBand = NULL;
    int nBandID = -1;

    for( int iBand=1; iBand <= nBands; iBand++ ) {
        CPLDebug( "GDAL_netCDF", "creating band # %d/%d nDim = %d",
                  iBand, nBands, nDim );

        poSrcBand = poSrcDS->GetRasterBand( iBand );
        eDT = poSrcBand->GetRasterDataType();

        /* Get var name from NETCDF_VARNAME */
        const char *tmpMetadata = poSrcBand->GetMetadataItem("NETCDF_VARNAME");
        char szBandName[ NC_MAX_NAME+1 ];
        if( tmpMetadata != NULL)
        {
            if( nBands > 1 && papszExtraDimNames == NULL ) 
                snprintf(szBandName,sizeof(szBandName),"%s%d",tmpMetadata,iBand);
            else
                snprintf(szBandName,sizeof(szBandName),"%s",tmpMetadata);
        }
        else
            szBandName[0]='\0';

        /* Get long_name from <var>#long_name */
        char szLongName[ NC_MAX_NAME+1 ];
        snprintf(szLongName, sizeof(szLongName),"%s#%s",
                poSrcBand->GetMetadataItem("NETCDF_VARNAME"),
                CF_LNG_NAME);
        tmpMetadata = poSrcDS->GetMetadataItem(szLongName);
        if( tmpMetadata != NULL) 
            snprintf( szLongName, sizeof(szLongName), "%s", tmpMetadata);
        else
            szLongName[0]='\0';

        bool bSignedData = true;
        if ( eDT == GDT_Byte ) {
            /* GDAL defaults to unsigned bytes, but check if metadata says its
               signed, as NetCDF can support this for certain formats. */
            bSignedData = false;
            tmpMetadata = poSrcBand->GetMetadataItem("PIXELTYPE",
                                                     "IMAGE_STRUCTURE");
            if ( tmpMetadata && EQUAL(tmpMetadata,"SIGNEDBYTE") )
                bSignedData = true;
        }

        if ( nDim > 2 )
            poBand = new netCDFRasterBand( poDS, eDT, iBand,
                                           bSignedData,
                                           szBandName, szLongName,
                                           nBandID, nDim, iBand-1,  
                                           panBandZLev, panBandDimPos, 
                                           panDimIds );
        else
            poBand = new netCDFRasterBand( poDS, eDT, iBand,
                                           bSignedData,
                                           szBandName, szLongName );

        poDS->SetBand( iBand, poBand );

        /* set nodata value, if any */
        // poBand->SetNoDataValue( poSrcBand->GetNoDataValue(0) );
        int bNoDataSet;
        double dfNoDataValue = poSrcBand->GetNoDataValue( &bNoDataSet );
        if ( bNoDataSet ) {
            CPLDebug( "GDAL_netCDF", "SetNoDataValue(%f) source", dfNoDataValue );
            poBand->SetNoDataValue( dfNoDataValue );
        }

        /* Copy Metadata for band */
        CopyMetadata( (void *) GDALGetRasterBand( poSrcDS, iBand ), 
                      poDS->cdfid, poBand->nZId );

        /* if more than 2D pass the first band's netcdf var ID to subsequent bands */
        if ( nDim > 2 )
            nBandID = poBand->nZId;
    }

    /* write projection variable to band variable */
    poDS->AddGridMappingRef();

    pfnProgress( 0.5, NULL, pProgressData );

/* -------------------------------------------------------------------- */
/*      Write Bands                                                     */
/* -------------------------------------------------------------------- */
    /* make sure we are in data mode */
    poDS->SetDefineMode( false );

    double dfTemp = 0.5;
    double dfTemp2 = 0.5;

    eErr = CE_None;
    GDALRasterBand *poDstBand = NULL;

    for( int iBand=1; iBand <= nBands && eErr == CE_None; iBand++ ) {
        dfTemp2 = dfTemp + 0.4/nBands; 
        pScaledProgress = 
            GDALCreateScaledProgress( dfTemp, dfTemp2,
                                      pfnProgress, pProgressData );
        dfTemp = dfTemp2;

        CPLDebug( "GDAL_netCDF", "copying band data # %d/%d ",
                  iBand,nBands );

        poSrcBand = poSrcDS->GetRasterBand( iBand );
        eDT = poSrcBand->GetRasterDataType();

        poDstBand = poDS->GetRasterBand( iBand );

/* -------------------------------------------------------------------- */
/*      Copy Band data                                                  */
/* -------------------------------------------------------------------- */
        if( eDT == GDT_Byte ) {
            CPLDebug( "GDAL_netCDF", "GByte Band#%d", iBand );
            eErr = NCDFCopyBand<GByte>( poSrcBand, poDstBand, nXSize, nYSize,
                                 GDALScaledProgress, pScaledProgress );
        }
        else if( ( eDT == GDT_UInt16 ) || ( eDT == GDT_Int16 ) ) {
            CPLDebug( "GDAL_netCDF", "GInt16 Band#%d", iBand );
            eErr = NCDFCopyBand<GInt16>( poSrcBand, poDstBand, nXSize, nYSize,
                                 GDALScaledProgress, pScaledProgress );
        }
        else if( (eDT == GDT_UInt32) || (eDT == GDT_Int32) ) {
            CPLDebug( "GDAL_netCDF", "GInt16 Band#%d", iBand );
            eErr = NCDFCopyBand<GInt32>( poSrcBand, poDstBand, nXSize, nYSize,
                                 GDALScaledProgress, pScaledProgress );
        }
        else if( eDT == GDT_Float32 ) {
            CPLDebug( "GDAL_netCDF", "float Band#%d", iBand);
            eErr = NCDFCopyBand<float>( poSrcBand, poDstBand, nXSize, nYSize,
                                 GDALScaledProgress, pScaledProgress );
        }
        else if( eDT == GDT_Float64 ) {
            CPLDebug( "GDAL_netCDF", "double Band#%d", iBand);
            eErr = NCDFCopyBand<double>( poSrcBand, poDstBand, nXSize, nYSize,
                                 GDALScaledProgress, pScaledProgress );
        }
        else {
            CPLError( CE_Failure, CPLE_NotSupported,
                      "The NetCDF driver does not support GDAL data type %d",
                      eDT );
        }

        GDALDestroyScaledProgress( pScaledProgress );
    }

/* -------------------------------------------------------------------- */
/*      Cleanup and close.                                              */
/* -------------------------------------------------------------------- */
    delete( poDS );

    CPLFree( panDimIds );
    CPLFree( panBandDimPos );
    CPLFree( panBandZLev );
    CPLFree( panDimVarIds );
    if ( papszExtraDimNames )
        CSLDestroy( papszExtraDimNames );

    if (eErr != CE_None)
        return NULL;

    pfnProgress( 0.95, NULL, pProgressData );

/* -------------------------------------------------------------------- */
/*      Re-open dataset so we can return it.                            */
/* -------------------------------------------------------------------- */
    poDS = reinterpret_cast<netCDFDataset *>(
        GDALOpen( pszFilename, GA_ReadOnly ) );

/* -------------------------------------------------------------------- */
/*      PAM cloning is disabled. See bug #4244.                         */
/* -------------------------------------------------------------------- */
    // if( poDS )
    //     poDS->CloneInfo( poSrcDS, GCIF_PAM_DEFAULT );

    pfnProgress( 1.0, NULL, pProgressData );

    return poDS;
}

/* note: some logic depends on bIsProjected and bIsGeoGraphic */
/* which may not be known when Create() is called, see AddProjectionVars() */
void
netCDFDataset::ProcessCreationOptions( )
{

    /* File format */
    eFormat = NCDF_FORMAT_NC;
    const char *pszValue = CSLFetchNameValue( papszCreationOptions, "FORMAT" );
    if ( pszValue != NULL ) {
        if ( EQUAL( pszValue, "NC" ) ) {
            eFormat = NCDF_FORMAT_NC;
        }
#ifdef NETCDF_HAS_NC2
        else if ( EQUAL( pszValue, "NC2" ) ) {
            eFormat = NCDF_FORMAT_NC2;
        }
#endif
#ifdef NETCDF_HAS_NC4
        else if ( EQUAL( pszValue, "NC4" ) ) {
            eFormat = NCDF_FORMAT_NC4;
        }
        else if ( EQUAL( pszValue, "NC4C" ) ) {
            eFormat = NCDF_FORMAT_NC4C;
        }
#endif
        else {
            CPLError( CE_Failure, CPLE_NotSupported,
                      "FORMAT=%s in not supported, using the default NC format.", pszValue );        
        }
    }

    /* compression only available for NC4 */
#ifdef NETCDF_HAS_NC4

    /* COMPRESS option */
    pszValue = CSLFetchNameValue( papszCreationOptions, "COMPRESS" );
    if ( pszValue != NULL ) {
        if ( EQUAL( pszValue, "NONE" ) ) {
            eCompress = NCDF_COMPRESS_NONE;
        }
        else if ( EQUAL( pszValue, "DEFLATE" ) ) {
            eCompress = NCDF_COMPRESS_DEFLATE;
            if ( !((eFormat == NCDF_FORMAT_NC4) || (eFormat == NCDF_FORMAT_NC4C)) ) {
                CPLError( CE_Warning, CPLE_IllegalArg,
                          "NOTICE: Format set to NC4C because compression is set to DEFLATE." );
                eFormat = NCDF_FORMAT_NC4C;
            }
        }
        else {
            CPLError( CE_Failure, CPLE_NotSupported,
                      "COMPRESS=%s is not supported.", pszValue );
        }
    }

    /* ZLEVEL option */
    pszValue = CSLFetchNameValue( papszCreationOptions, "ZLEVEL" );
    if( pszValue != NULL )
    {
        nZLevel =  atoi( pszValue );
        if (!(nZLevel >= 1 && nZLevel <= 9))
        {
            CPLError( CE_Warning, CPLE_IllegalArg, 
                    "ZLEVEL=%s value not recognised, ignoring.",
                    pszValue );
            nZLevel = NCDF_DEFLATE_LEVEL;
        }
    }

    /* CHUNKING option */
    bChunking = CPL_TO_BOOL(CSLFetchBoolean( papszCreationOptions, "CHUNKING", TRUE ));

#endif

    /* set nCreateMode based on eFormat */
    switch ( eFormat ) {
#ifdef NETCDF_HAS_NC2
        case NCDF_FORMAT_NC2:
            nCreateMode = NC_CLOBBER|NC_64BIT_OFFSET;
            break;
#endif
#ifdef NETCDF_HAS_NC4
        case NCDF_FORMAT_NC4:
            nCreateMode = NC_CLOBBER|NC_NETCDF4;
            break;
        case NCDF_FORMAT_NC4C:
            nCreateMode = NC_CLOBBER|NC_NETCDF4|NC_CLASSIC_MODEL;
            break;
#endif
        case NCDF_FORMAT_NC:
        default:
            nCreateMode = NC_CLOBBER;
            break;
    }

    CPLDebug( "GDAL_netCDF", 
              "file options: format=%d compress=%d zlevel=%d",
              eFormat, eCompress, nZLevel );
}

int netCDFDataset::DefVarDeflate(
#ifdef NETCDF_HAS_NC4
            int nVarId, bool bChunkingArg
#else
            int /* nVarId */ , bool /* bChunkingArg */
#endif
            )
{
#ifdef NETCDF_HAS_NC4
    if ( eCompress == NCDF_COMPRESS_DEFLATE ) {
        // Must set chunk size to avoid huge performance hit (set bChunkingArg=TRUE)
        // perhaps another solution it to change the chunk cache?
        // http://www.unidata.ucar.edu/software/netcdf/docs/netcdf.html#Chunk-Cache   
        // TODO: make sure this is okay.
        CPLDebug( "GDAL_netCDF",
                  "DefVarDeflate( %d, %d ) nZlevel=%d",
                  nVarId, static_cast<int>(bChunkingArg), nZLevel );

        int status = nc_def_var_deflate(cdfid,nVarId,1,1,nZLevel);
        NCDF_ERR(status);

        if ( (status == NC_NOERR) && bChunkingArg && bChunking ) {

            // set chunking to be 1 for all dims, except X dim
            // size_t chunksize[] = { 1, (size_t)nRasterXSize };
            size_t chunksize[ MAX_NC_DIMS ];
            int nd;
            nc_inq_varndims( cdfid, nVarId, &nd );
            chunksize[0] = (size_t)1;
            chunksize[1] = (size_t)1;
            for( int i=2; i<nd; i++ ) chunksize[i] = (size_t)1;
            chunksize[nd-1] = (size_t)nRasterXSize;

            CPLDebug( "GDAL_netCDF", 
                      "DefVarDeflate() chunksize={%ld, %ld} chunkX=%ld nd=%d",
                      (long)chunksize[0], (long)chunksize[1], (long)chunksize[nd-1], nd );
#ifdef NCDF_DEBUG
            for( int i=0; i<nd; i++ ) 
                CPLDebug( "GDAL_netCDF","DefVarDeflate() chunk[%d]=%ld", i, chunksize[i] );
#endif

            status = nc_def_var_chunking( cdfid, nVarId,          
                                          NC_CHUNKED, chunksize );
            NCDF_ERR(status);
        }
        else {
            CPLDebug( "GDAL_netCDF", 
                      "chunksize not set" );
        }
        return status;
    }
#endif
    return NC_NOERR;
}

/************************************************************************/
/*                           NCDFUnloadDriver()                         */
/************************************************************************/

static void NCDFUnloadDriver(CPL_UNUSED GDALDriver* poDriver)
{
    if( hNCMutex != NULL )
        CPLDestroyMutex(hNCMutex);
    hNCMutex = NULL;
}

/************************************************************************/
/*                          GDALRegister_netCDF()                       */
/************************************************************************/

void GDALRegister_netCDF()

{
    if( !GDAL_CHECK_VERSION( "netCDF driver" ) )
        return;

    if( GDALGetDriverByName( "netCDF" ) != NULL )
        return;

    GDALDriver *poDriver = new GDALDriver( );

/* -------------------------------------------------------------------- */
/*      Set the driver details.                                         */
/* -------------------------------------------------------------------- */
    poDriver->SetDescription( "netCDF" );
    poDriver->SetMetadataItem( GDAL_DCAP_RASTER, "YES" );
    poDriver->SetMetadataItem( GDAL_DCAP_VECTOR, "YES" );
    poDriver->SetMetadataItem( GDAL_DMD_LONGNAME,
                               "Network Common Data Format" );
    poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC,
                               "frmt_netcdf.html" );
    poDriver->SetMetadataItem( GDAL_DMD_EXTENSION, "nc" );
    poDriver->SetMetadataItem( GDAL_DMD_CREATIONOPTIONLIST,
"<CreationOptionList>"
"   <Option name='FORMAT' type='string-select' default='NC'>"
"     <Value>NC</Value>"
#ifdef NETCDF_HAS_NC2
"     <Value>NC2</Value>"
#endif
#ifdef NETCDF_HAS_NC4
"     <Value>NC4</Value>"
"     <Value>NC4C</Value>"
#endif
"   </Option>"
#ifdef NETCDF_HAS_NC4
"   <Option name='COMPRESS' type='string-select' default='NONE'>"
"     <Value>NONE</Value>"
"     <Value>DEFLATE</Value>"
"   </Option>"
"   <Option name='ZLEVEL' type='int' description='DEFLATE compression level 1-9' default='1'/>"
#endif
"   <Option name='WRITE_BOTTOMUP' type='boolean' default='YES'>"
"   </Option>"
"   <Option name='WRITE_GDAL_TAGS' type='boolean' default='YES'>"
"   </Option>"
"   <Option name='WRITE_LONLAT' type='string-select'>"
"     <Value>YES</Value>"
"     <Value>NO</Value>"
"     <Value>IF_NEEDED</Value>"
"   </Option>"
"   <Option name='TYPE_LONLAT' type='string-select'>"
"     <Value>float</Value>"
"     <Value>double</Value>"
"   </Option>"
"   <Option name='PIXELTYPE' type='string-select' description='only used in Create()'>"
"       <Value>DEFAULT</Value>"
"       <Value>SIGNEDBYTE</Value>"
"   </Option>"
"   <Option name='CHUNKING' type='boolean' default='YES' description='define chunking when creating netcdf4 file'>"
"   </Option>"
"</CreationOptionList>"
                               );
    poDriver->SetMetadataItem( GDAL_DMD_SUBDATASETS, "YES" );

    poDriver->SetMetadataItem( GDAL_DS_LAYER_CREATIONOPTIONLIST,
"<LayerCreationOptionList>"
"   <Option name='RECORD_DIM_NAME' type='string' description='Name of the unlimited dimension' default='record'/>"
"   <Option name='STRING_MAX_WIDTH' type='string' description='"
#ifdef NETCDF_HAS_NC4
"For non-NC4 format, "
#endif
"default maximum width of strings' default='80'/>"
#ifdef NETCDF_HAS_NC4
"   <Option name='USE_STRING_IN_NC4'  type='boolean' description='Whether to use NetCDF string type for strings in NC4 format. If NO, bidimensional char variable are used' default='YES'/>"
#endif
"</LayerCreationOptionList>" );

    /* make driver config and capabilities available */
    poDriver->SetMetadataItem( "NETCDF_VERSION", nc_inq_libvers() );
    poDriver->SetMetadataItem( "NETCDF_CONVENTIONS", NCDF_CONVENTIONS_CF_V1_5 );
#ifdef NETCDF_HAS_NC2
    poDriver->SetMetadataItem( "NETCDF_HAS_NC2", "YES" );
#endif
#ifdef NETCDF_HAS_NC4
    poDriver->SetMetadataItem( "NETCDF_HAS_NC4", "YES" );
#endif
#ifdef NETCDF_HAS_HDF4
    poDriver->SetMetadataItem( "NETCDF_HAS_HDF4", "YES" );
#endif
#ifdef HAVE_HDF4
    poDriver->SetMetadataItem( "GDAL_HAS_HDF4", "YES" );
#endif
#ifdef HAVE_HDF5
    poDriver->SetMetadataItem( "GDAL_HAS_HDF5", "YES" );
#endif

    poDriver->SetMetadataItem( GDAL_DMD_CREATIONFIELDDATATYPES,
                               "Integer Integer64 Real String Date DateTime" );

    /* set pfns and register driver */
    poDriver->pfnOpen = netCDFDataset::Open;
    poDriver->pfnCreateCopy = netCDFDataset::CreateCopy;
    poDriver->pfnCreate = netCDFDataset::Create;
    poDriver->pfnIdentify = netCDFDataset::Identify;
    poDriver->pfnUnloadDriver = NCDFUnloadDriver;

    GetGDALDriverManager( )->RegisterDriver( poDriver );

#ifdef NETCDF_PLUGIN
    GDALRegister_GMT();
#endif
}

/************************************************************************/
/*                          New functions                               */
/************************************************************************/

/* Test for GDAL version string >= target */
static bool NCDFIsGDALVersionGTE(const char* pszVersion, int nTarget)
{

    /* Valid strings are "GDAL 1.9dev, released 2011/01/18" and "GDAL 1.8.1 " */
    if ( pszVersion == NULL || EQUAL( pszVersion, "" ) )
        return false;
    else if ( ! STARTS_WITH_CI(pszVersion, "GDAL ") )
        return false;
    /* 2.0dev of 2011/12/29 has been later renamed as 1.10dev */
    else if ( EQUAL("GDAL 2.0dev, released 2011/12/29", pszVersion) )
        return nTarget <= GDAL_COMPUTE_VERSION(1,10,0);
    else if ( STARTS_WITH_CI(pszVersion, "GDAL 1.9dev") )
        return nTarget <= 1900;
    else if ( STARTS_WITH_CI(pszVersion, "GDAL 1.8dev") )
        return nTarget <= 1800;

    char **papszTokens = CSLTokenizeString2( pszVersion+5, ".", 0 );

    int nVersions [] = {0, 0, 0, 0};
    for ( int iToken = 0; papszTokens && iToken < 4 && papszTokens[iToken]; iToken++ )  {
        nVersions[iToken] = atoi( papszTokens[iToken] );
    }

    int nVersion = 0;
    if( nVersions[0] > 1 || nVersions[1] >= 10 )
        nVersion = GDAL_COMPUTE_VERSION( nVersions[0], nVersions[1], nVersions[2] );
    else
        nVersion = nVersions[0]*1000 + nVersions[1]*100 + 
            nVersions[2]*10 + nVersions[3]; 

    CSLDestroy( papszTokens );
    return nTarget <= nVersion;
}

/* Add Conventions, GDAL version and history  */ 
static void NCDFAddGDALHistory( int fpImage, 
                         const char * pszFilename, const char *pszOldHist,
                         const char * pszFunctionName,
                                const char * pszCFVersion )
{
    nc_put_att_text( fpImage, NC_GLOBAL, "Conventions", 
                     strlen(pszCFVersion),
                     pszCFVersion ); 

    const char* pszNCDF_GDAL = GDALVersionInfo("--version");
    nc_put_att_text( fpImage, NC_GLOBAL, "GDAL", 
                     strlen(pszNCDF_GDAL), pszNCDF_GDAL );

    /* Add history */
    CPLString osTmp;
#ifdef GDAL_SET_CMD_LINE_DEFINED_TMP
    if ( ! EQUAL(GDALGetCmdLine(), "" ) )
        osTmp = GDALGetCmdLine();
    else
        osTmp = CPLSPrintf("GDAL %s( %s, ... )",pszFunctionName,pszFilename );
#else
    osTmp = CPLSPrintf("GDAL %s( %s, ... )",pszFunctionName,pszFilename );
#endif

    NCDFAddHistory( fpImage, osTmp.c_str(), pszOldHist );
}

/* code taken from cdo and libcdi, used for writing the history attribute */
//void cdoDefHistory(int fileID, char *histstring)
static void NCDFAddHistory(int fpImage, const char *pszAddHist, const char *pszOldHist)
{
    /* Check pszOldHist - as if there was no previous history, it will be
       a null pointer - if so set as empty. */
    if (NULL == pszOldHist) {
        pszOldHist = "";
    }

    char strtime[32];
    strtime[0] = '\0';

    time_t tp = time(NULL);
    if ( tp != -1 )
    {
        struct tm *ltime = localtime(&tp);
        (void) strftime(strtime, sizeof(strtime), "%a %b %d %H:%M:%S %Y: ", ltime);
    }

    // status = nc_get_att_text( fpImage, NC_GLOBAL,
    //                           "history", pszOldHist );
    // printf("status: %d pszOldHist: [%s]\n",status,pszOldHist);

    size_t nNewHistSize
        = strlen(pszOldHist)+strlen(strtime)+strlen(pszAddHist)+1+1;
    char *pszNewHist
        = reinterpret_cast<char *>( CPLMalloc(nNewHistSize * sizeof(char)) );

    strcpy(pszNewHist, strtime);
    strcat(pszNewHist, pszAddHist);

    // int disableHistory = FALSE;
    //if ( !disableHistory )
    {
        if ( ! EQUAL(pszOldHist,"") )
            strcat(pszNewHist, "\n");
        strcat(pszNewHist, pszOldHist);
    }

    int status = nc_put_att_text( fpImage, NC_GLOBAL,
                                  "history", strlen(pszNewHist),
                                  pszNewHist );
    NCDF_ERR(status);

    CPLFree(pszNewHist);
}

static bool NCDFIsCfProjection( const char* pszProjection ) 
{
    /* Find the appropriate mapping */
    for (int iMap = 0; poNetcdfSRS_PT[iMap].WKT_SRS != NULL; iMap++ ) {
        // printf("now at %d, proj=%s\n",i, poNetcdfSRS_PT[i].GDAL_SRS);
        if ( EQUAL( pszProjection, poNetcdfSRS_PT[iMap].WKT_SRS ) )  {
            return poNetcdfSRS_PT[iMap].mappings != NULL;
        }
    }
    return false;
}


/* Write any needed projection attributes *
 * poPROJCS: ptr to proj crd system
 * pszProjection: name of projection system in GDAL WKT
 * fpImage: open NetCDF file in writing mode
 * NCDFVarID: NetCDF Var Id of proj system we're writing in to
 *
 * The function first looks for the oNetcdfSRS_PP mapping object
 * that corresponds to the input projection name. If none is found
 * the generic mapping is used.  In the case of specific mappings,
 * the driver looks for each attribute listed in the mapping object
 * and then looks up the value within the OGR_SRSNode. In the case
 * of the generic mapping, the lookup is reversed (projection params, 
 * then mapping).  For more generic code, GDAL->NETCDF 
 * mappings and the associated value are saved in std::map objects.
 */

/* NOTE modifications by ET to combine the specific and generic mappings */

static void NCDFWriteProjAttribs( const OGR_SRSNode *poPROJCS,
                           const char* pszProjection,
                           const int fpImage, const int NCDFVarID ) 
{
    const oNetcdfSRS_PP *poMap = NULL;
    int nMapIndex = -1;

    /* Find the appropriate mapping */
    for (int iMap = 0; poNetcdfSRS_PT[iMap].WKT_SRS != NULL; iMap++ ) {
        if ( EQUAL( pszProjection, poNetcdfSRS_PT[iMap].WKT_SRS ) ) {
            nMapIndex = iMap;
            poMap = poNetcdfSRS_PT[iMap].mappings;
            break;
        }
    }

    //ET TODO if projection name is not found, should we do something special?
    if ( nMapIndex == -1 ) {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "projection name %s not found in the lookup tables!!!",
                  pszProjection);
    }
    /* if no mapping was found or assigned, set the generic one */
    if ( !poMap ) {
        CPLError( CE_Warning, CPLE_AppDefined, 
                  "projection name %s in not part of the CF standard, will not be supported by CF!",
                  pszProjection);
        poMap = poGenericMappings;
    }

    /* initialize local map objects */

    // Attribute <GDAL,NCDF> and Value <NCDF,value> mappings
    std::map< std::string, std::string > oAttMap;
    for ( int iMap = 0; poMap[iMap].WKT_ATT != NULL; iMap++ ) {
        oAttMap[poMap[iMap].WKT_ATT] = poMap[iMap].CF_ATT;
    }

    const char *pszParamStr;
    const char *pszParamVal;
    std::map< std::string, double > oValMap;
    for( int iChild = 0; iChild < poPROJCS->GetChildCount(); iChild++ ) {

        const OGR_SRSNode *poNode = poPROJCS->GetChild( iChild );
        if( !EQUAL(poNode->GetValue(),"PARAMETER") 
            || poNode->GetChildCount() != 2 )
            continue;
        pszParamStr = poNode->GetChild(0)->GetValue();
        pszParamVal = poNode->GetChild(1)->GetValue();

        oValMap[pszParamStr] = CPLAtof(pszParamVal);
    }

    double dfValue = 0.0;
    const std::string *posNCDFAtt;
    const std::string *posGDALAtt;
    bool bWriteVal = false;
    std::map< std::string, std::string >::iterator oAttIter;
    std::map< std::string, double >::iterator oValIter, oValIter2;

    // Results to write.
    std::vector< std::pair<std::string,double> > oOutList;

    /* Lookup mappings and fill output vector */
    if ( poMap != poGenericMappings ) { /* specific mapping, loop over mapping values */

        for ( oAttIter = oAttMap.begin(); oAttIter != oAttMap.end(); oAttIter++ ) {

            posGDALAtt = &(oAttIter->first);
            posNCDFAtt = &(oAttIter->second);
            oValIter = oValMap.find( *posGDALAtt );

            if ( oValIter != oValMap.end() ) {

                dfValue = oValIter->second;
                bWriteVal = true;

                /* special case for PS (Polar Stereographic) grid
                   See comments in netcdfdataset.h for this projection. */
                if ( EQUAL( SRS_PP_LATITUDE_OF_ORIGIN, posGDALAtt->c_str() ) &&
                     EQUAL(pszProjection, SRS_PT_POLAR_STEREOGRAPHIC) ) {
                    double dfLatPole = 0.0;
                    if ( dfValue > 0.0) dfLatPole = 90.0;
                    else dfLatPole = -90.0;
                        oOutList.push_back( std::make_pair( std::string(CF_PP_LAT_PROJ_ORIGIN), 
                                                            dfLatPole ) );
                }

                /* special case for LCC-1SP
                   See comments in netcdfdataset.h for this projection. */
                else if ( EQUAL( SRS_PP_SCALE_FACTOR, posGDALAtt->c_str() ) &&
                          EQUAL(pszProjection, SRS_PT_LAMBERT_CONFORMAL_CONIC_1SP) ) {
                    /* default is to not write as it is not CF-1 */
                    bWriteVal = false;
                    /* test if there is no standard_parallel1 */
                    if ( oValMap.find( std::string(CF_PP_STD_PARALLEL_1) ) == oValMap.end() ) {
                        /* if scale factor != 1.0  write value for GDAL, but this is not supported by CF-1 */
                        if ( !CPLIsEqual(dfValue,1.0) ) {
                            CPLError( CE_Failure, CPLE_NotSupported, 
                                      "NetCDF driver export of LCC-1SP with scale factor != 1.0 "
                                      "and no standard_parallel1 is not CF-1 (bug #3324).\n" 
                                      "Use the 2SP variant which is supported by CF." );   
                            bWriteVal = true;
                        }
                        /* else copy standard_parallel1 from latitude_of_origin, because scale_factor=1.0 */
                        else {
                            oValIter2 = oValMap.find( std::string(SRS_PP_LATITUDE_OF_ORIGIN) );
                            if (oValIter2 != oValMap.end() ) {
                                oOutList.push_back( std::make_pair( std::string(CF_PP_STD_PARALLEL_1), 
                                                                    oValIter2->second) );
                            }
                            else {
                                CPLError( CE_Failure, CPLE_NotSupported, 
                                          "NetCDF driver export of LCC-1SP with no standard_parallel1 "
                                          "and no latitude_of_origin is not supported (bug #3324).");
                            }
                        }
                    }
                }
                if ( bWriteVal )
                    oOutList.push_back( std::make_pair( *posNCDFAtt, dfValue ) );

            }
            // else printf("NOT FOUND!!!\n");
        }

    }
    else { /* generic mapping, loop over projected values */

        for ( oValIter = oValMap.begin(); oValIter != oValMap.end(); oValIter++ ) {

            posGDALAtt = &(oValIter->first);
            dfValue = oValIter->second;

            oAttIter = oAttMap.find( *posGDALAtt );

            if ( oAttIter != oAttMap.end() ) {
                oOutList.push_back( std::make_pair( oAttIter->second, dfValue ) );
            }
            /* for SRS_PP_SCALE_FACTOR write 2 mappings */
            else if (  EQUAL(posGDALAtt->c_str(), SRS_PP_SCALE_FACTOR) ) {
                oOutList.push_back( std::make_pair( std::string(CF_PP_SCALE_FACTOR_MERIDIAN),
                                                    dfValue ) );
                oOutList.push_back( std::make_pair( std::string(CF_PP_SCALE_FACTOR_ORIGIN),
                                                    dfValue ) );
            }
            /* if not found insert the GDAL name */
            else {
                oOutList.push_back( std::make_pair( *posGDALAtt, dfValue ) );
            }
        }
    }

    /* Write all the values that were found */
    // std::vector< std::pair<std::string,double> >::reverse_iterator it;
    // for (it = oOutList.rbegin();  it != oOutList.rend(); it++ ) {
    std::vector< std::pair<std::string,double> >::iterator it;
    double dfStdP[2];
    bool bFoundStdP1 = false;
    bool bFoundStdP2 = false;
    for (it = oOutList.begin();  it != oOutList.end(); it++ ) {
        pszParamVal = (it->first).c_str();
        dfValue = it->second;
        /* Handle the STD_PARALLEL attrib */
        if( EQUAL( pszParamVal, CF_PP_STD_PARALLEL_1 ) ) {
            bFoundStdP1 = true;
            dfStdP[0] = dfValue;
        }
        else if( EQUAL( pszParamVal, CF_PP_STD_PARALLEL_2 ) ) {
            bFoundStdP2 = true;
            dfStdP[1] = dfValue;
        }
        else {
            nc_put_att_double( fpImage, NCDFVarID, pszParamVal,
                               NC_DOUBLE, 1,&dfValue );
        }
    }
    /* Now write the STD_PARALLEL attrib */
    if ( bFoundStdP1 ) {
        /* one value or equal values */
        if ( !bFoundStdP2 || dfStdP[0] ==  dfStdP[1] ) {
            nc_put_att_double( fpImage, NCDFVarID, CF_PP_STD_PARALLEL, 
                               NC_DOUBLE, 1, &dfStdP[0] );
        }
        else { /* two values */
            nc_put_att_double( fpImage, NCDFVarID, CF_PP_STD_PARALLEL, 
                               NC_DOUBLE, 2, dfStdP );
        }
    }
}

static CPLErr NCDFSafeStrcat(char** ppszDest, const char* pszSrc, size_t* nDestSize)
{
    /* Reallocate the data string until the content fits */
    while(*nDestSize < (strlen(*ppszDest) + strlen(pszSrc) + 1)) {
        (*nDestSize) *= 2;
        *ppszDest = reinterpret_cast<char *>(
            CPLRealloc( reinterpret_cast<void *>( *ppszDest ), *nDestSize) );
#ifdef NCDF_DEBUG
        CPLDebug( "GDAL_netCDF", "NCDFSafeStrcat() resized str from %ld to %ld", (*nDestSize)/2, *nDestSize );
#endif
    }
    strcat(*ppszDest, pszSrc);

    return CE_None;
}

/* helper function for NCDFGetAttr() */
/* sets pdfValue to first value returned */
/* and if bSetPszValue=True sets pszValue with all attribute values */
/* pszValue is the responsibility of the caller and must be freed */
static CPLErr NCDFGetAttr1( int nCdfId, int nVarId, const char *pszAttrName, 
                     double *pdfValue, char **pszValue, int bSetPszValue )
{
    nc_type nAttrType = NC_NAT;
    size_t  nAttrLen = 0;

    int status = nc_inq_att( nCdfId, nVarId, pszAttrName, &nAttrType, &nAttrLen);
    if ( status != NC_NOERR )
        return CE_Failure;

#ifdef NCDF_DEBUG
    CPLDebug( "GDAL_netCDF", "NCDFGetAttr1(%s) len=%ld type=%d", pszAttrName, nAttrLen, nAttrType );
#endif

    /* Allocate guaranteed minimum size (use 10 or 20 if not a string) */
    size_t  nAttrValueSize = nAttrLen + 1;
    if ( nAttrType != NC_CHAR && nAttrValueSize < 10 )
        nAttrValueSize = 10;
    if ( nAttrType == NC_DOUBLE && nAttrValueSize < 20 )
        nAttrValueSize = 20;
#ifdef NETCDF_HAS_NC4
    if ( nAttrType == NC_INT64 && nAttrValueSize < 20 )
        nAttrValueSize = 22;
#endif
    char *pszAttrValue = (char *) CPLCalloc( nAttrValueSize, sizeof( char ));
    *pszAttrValue = '\0';

    if ( nAttrLen > 1 && nAttrType != NC_CHAR )
        NCDFSafeStrcat(&pszAttrValue, "{", &nAttrValueSize);

    double dfValue = 0.0;
    size_t m;
    char szTemp[ 256 ];

    switch (nAttrType) {
        case NC_CHAR:
            nc_get_att_text( nCdfId, nVarId, pszAttrName, pszAttrValue );
            pszAttrValue[nAttrLen]='\0';
            dfValue = 0.0;
            break;
        case NC_BYTE:
        {
            signed char *pscTemp
                = reinterpret_cast<signed char *>(
                    CPLCalloc( nAttrLen, sizeof( signed char ) ) );
            nc_get_att_schar( nCdfId, nVarId, pszAttrName, pscTemp );
            dfValue = static_cast<double>( pscTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pscTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pscTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(pscTemp);
            break;
        }
#ifdef NETCDF_HAS_NC4
        case NC_UBYTE:
        {
            unsigned char *pucTemp
                = reinterpret_cast<unsigned char *>(
                    CPLCalloc( nAttrLen, sizeof( unsigned char ) ) );
            nc_get_att_uchar( nCdfId, nVarId, pszAttrName, pucTemp );
            dfValue = static_cast<double>( pucTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pucTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pucTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(pucTemp);
            break;
        }
        case NC_USHORT:
        {
            unsigned short *pusTemp;
            pusTemp = reinterpret_cast<unsigned short *>(
                    CPLCalloc( nAttrLen, sizeof( unsigned short ) ) );
            nc_get_att_ushort( nCdfId, nVarId, pszAttrName, pusTemp );
            dfValue = static_cast<double>(pusTemp[0]);
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pusTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pusTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(pusTemp);
            break;
        }
#endif
        case NC_SHORT:
        {
            short *psTemp
                = reinterpret_cast<short *>(
                    CPLCalloc( nAttrLen, sizeof( short ) ) );
            nc_get_att_short( nCdfId, nVarId, pszAttrName, psTemp );
            dfValue = static_cast<double>( psTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%hd,", psTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%hd", psTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(psTemp);
            break;
        }
        case NC_INT:
        {
            int *pnTemp
                = static_cast<int *>( CPLCalloc( nAttrLen, sizeof( int ) ) );
            nc_get_att_int( nCdfId, nVarId, pszAttrName, pnTemp );
            dfValue = static_cast<double>( pnTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pnTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pnTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(pnTemp);
            break;
        }
#ifdef NETCDF_HAS_NC4
        case NC_UINT:
        {
            unsigned int *punTemp
                = static_cast<unsigned int *>( CPLCalloc( nAttrLen, sizeof( int ) ) );
            nc_get_att_uint( nCdfId, nVarId, pszAttrName, punTemp );
            dfValue = static_cast<double>( punTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%u,", punTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%u", punTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(punTemp);
            break;
        }

        case NC_INT64:
        {
            GIntBig *panTemp
                = reinterpret_cast<GIntBig *>(
                    CPLCalloc( nAttrLen, sizeof( GIntBig ) ) );
            nc_get_att_longlong( nCdfId, nVarId, pszAttrName, panTemp );
            dfValue = static_cast<double>( panTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), CPL_FRMT_GIB ",", panTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), CPL_FRMT_GIB, panTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(panTemp);
            break;
        }
#endif
        case NC_FLOAT:
        {
            float *pfTemp
                = reinterpret_cast<float *>(
                    CPLCalloc( nAttrLen, sizeof( float ) ) );
            nc_get_att_float( nCdfId, nVarId, pszAttrName, pfTemp );
            dfValue = static_cast<double>( pfTemp[0] );
            for(m=0; m < nAttrLen-1; m++) {
                CPLsnprintf( szTemp, sizeof(szTemp), "%.8g,", pfTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            CPLsnprintf( szTemp, sizeof(szTemp), "%.8g", pfTemp[m] );
            NCDFSafeStrcat(&pszAttrValue,szTemp, &nAttrValueSize);
            CPLFree(pfTemp);
            break;
        }
        case NC_DOUBLE:
        {
            double *pdfTemp
                = reinterpret_cast<double *>(
                    CPLCalloc( nAttrLen, sizeof(double) ) );
            nc_get_att_double( nCdfId, nVarId, pszAttrName, pdfTemp );
            dfValue = pdfTemp[0];
            for(m=0; m < nAttrLen-1; m++) {
                CPLsnprintf( szTemp, sizeof(szTemp), "%.16g,", pdfTemp[m] );
                NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            }
            CPLsnprintf( szTemp, sizeof(szTemp), "%.16g", pdfTemp[m] );
            NCDFSafeStrcat(&pszAttrValue, szTemp, &nAttrValueSize);
            CPLFree(pdfTemp);
            break;
        }
        default:
            CPLDebug( "GDAL_netCDF", "NCDFGetAttr unsupported type %d for attribute %s",
                      nAttrType,pszAttrName);
            break;
    }

    if ( nAttrLen > 1  && nAttrType!= NC_CHAR )
        NCDFSafeStrcat(&pszAttrValue, "}", &nAttrValueSize);

    /* set return values */
    if ( bSetPszValue ) *pszValue = pszAttrValue;
    else CPLFree ( pszAttrValue );
    if ( pdfValue ) *pdfValue = dfValue;

    return CE_None;
}

/* sets pdfValue to first value found */
static CPLErr NCDFGetAttr( int nCdfId, int nVarId, const char *pszAttrName, 
                    double *pdfValue )
{
    return NCDFGetAttr1( nCdfId, nVarId, pszAttrName, pdfValue, NULL, false );
}


/* pszValue is the responsibility of the caller and must be freed */
static CPLErr NCDFGetAttr( int nCdfId, int nVarId, const char *pszAttrName, 
                    char **pszValue )
{
    return NCDFGetAttr1( nCdfId, nVarId, pszAttrName, NULL, pszValue, true );
}


/* By default write NC_CHAR, but detect for int/float/double */
static CPLErr NCDFPutAttr( int nCdfId, int nVarId, 
                 const char *pszAttrName, const char *pszValue )
{
    int     status = 0;
    char    *pszTemp = NULL;

    /* get the attribute values as tokens */
    char **papszValues = NCDFTokenizeArray( pszValue );
    if ( papszValues == NULL ) 
        return CE_Failure;

    size_t nAttrLen = CSLCount(papszValues);

    /* first detect type */
    nc_type nAttrType = NC_CHAR;
    nc_type nTmpAttrType = NC_CHAR;
    for ( size_t i=0; i<nAttrLen; i++ ) {
        nTmpAttrType = NC_CHAR;
        errno = 0;
        CPL_IGNORE_RET_VAL(strtol( papszValues[i], &pszTemp, 10 ));
        /* test for int */
        /* TODO test for Byte and short - can this be done safely? */
        if ( (errno == 0) && (papszValues[i] != pszTemp) && (*pszTemp == 0) ) {
            nTmpAttrType = NC_INT;
        }
        else {
            /* test for double */
            errno = 0;
            double dfValue = CPLStrtod( papszValues[i], &pszTemp );
            if ( (errno == 0) && (papszValues[i] != pszTemp) && (*pszTemp == 0) ) {
                /* test for float instead of double */
                /* strtof() is C89, which is not available in MSVC */
                /* see if we loose precision if we cast to float and write to char* */
                float fValue = float(dfValue);
                char    szTemp[ 256 ];
                CPLsnprintf( szTemp, sizeof(szTemp), "%.8g",fValue);
                if ( EQUAL(szTemp, papszValues[i] ) )
                    nTmpAttrType = NC_FLOAT;
                else
                    nTmpAttrType = NC_DOUBLE;
            }
        }
        if ( nTmpAttrType > nAttrType )
            nAttrType = nTmpAttrType;
    }

    /* now write the data */
    if ( nAttrType == NC_CHAR ) {
        status = nc_put_att_text( nCdfId, nVarId, pszAttrName,
                                  strlen( pszValue ), pszValue );
        NCDF_ERR(status);
    }
    else {
        switch( nAttrType ) {
            case  NC_INT:
            {
                int *pnTemp = reinterpret_cast<int *> (
                    CPLCalloc( nAttrLen, sizeof( int ) ) );
                for( size_t i=0; i < nAttrLen; i++) {
                    pnTemp[i] = static_cast<int>(strtol( papszValues[i], &pszTemp, 10 ));
                }
                status = nc_put_att_int( nCdfId, nVarId, pszAttrName, 
                                         NC_INT, nAttrLen, pnTemp );  
                NCDF_ERR(status);
                CPLFree(pnTemp);
                break;
            }
            case  NC_FLOAT:
            {
                float *pfTemp = reinterpret_cast<float *> (
                    CPLCalloc( nAttrLen, sizeof( float ) ) );
                for( size_t i=0; i < nAttrLen; i++) {
                    pfTemp[i] = (float)CPLStrtod( papszValues[i], &pszTemp );
                }
                status = nc_put_att_float( nCdfId, nVarId, pszAttrName, 
                                           NC_FLOAT, nAttrLen, pfTemp );  
                NCDF_ERR(status);
                CPLFree(pfTemp);
                break;
            }
            case  NC_DOUBLE:
            {
                double *pdfTemp = reinterpret_cast<double *> (
                    CPLCalloc( nAttrLen, sizeof( double ) ) );
                for(size_t i=0; i < nAttrLen; i++) {
                    pdfTemp[i] = CPLStrtod( papszValues[i], &pszTemp );
                }
                status = nc_put_att_double( nCdfId, nVarId, pszAttrName, 
                                            NC_DOUBLE, nAttrLen, pdfTemp );
                NCDF_ERR(status);
                CPLFree(pdfTemp);
                break;
            }
        default:
            if ( papszValues ) CSLDestroy( papszValues );
            return CE_Failure;
            break;
        }
    }

    if ( papszValues ) CSLDestroy( papszValues );

     return CE_None;
}

static CPLErr NCDFGet1DVar( int nCdfId, int nVarId, char **pszValue )
{
    /* get var information */
    int nVarDimId = -1;
    int status = nc_inq_varndims( nCdfId, nVarId, &nVarDimId );
    if ( status != NC_NOERR || nVarDimId != 1)
        return CE_Failure;

    status = nc_inq_vardimid( nCdfId, nVarId, &nVarDimId );
    if ( status != NC_NOERR )
        return CE_Failure;

    nc_type nVarType = NC_NAT;
    status = nc_inq_vartype( nCdfId, nVarId, &nVarType );
    if ( status != NC_NOERR )
        return CE_Failure;

    size_t nVarLen = 0;
    status = nc_inq_dimlen( nCdfId, nVarDimId, &nVarLen );
    if ( status != NC_NOERR )
        return CE_Failure;

    size_t start[1] = {0};
    size_t count[1] = {nVarLen};

    /* Allocate guaranteed minimum size */
    size_t nVarValueSize = NCDF_MAX_STR_LEN;
    char *pszVarValue = reinterpret_cast<char *> (
        CPLCalloc( nVarValueSize, sizeof( char ) ) );
    *pszVarValue = '\0';

    if ( nVarLen > 1 && nVarType != NC_CHAR )    
        NCDFSafeStrcat(&pszVarValue, "{", &nVarValueSize);

    switch (nVarType) {
        case NC_CHAR:
            nc_get_vara_text( nCdfId, nVarId, start, count, pszVarValue );
            pszVarValue[nVarLen]='\0';
            break;
        /* TODO support NC_UBYTE */
        case NC_BYTE:
        {
            signed char *pscTemp = reinterpret_cast<signed char *> (
                CPLCalloc( nVarLen, sizeof( signed char ) ) );
            nc_get_vara_schar( nCdfId, nVarId, start, count, pscTemp );
            char szTemp[ 256 ];
            size_t m = 0;
            for( ; m < nVarLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pscTemp[m] );
                NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pscTemp[m] );
            NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            CPLFree(pscTemp);
            break;
        }
        case NC_SHORT:
        {
            short *psTemp = reinterpret_cast<short *> (
                CPLCalloc( nVarLen, sizeof( short ) ) );
            nc_get_vara_short( nCdfId, nVarId, start, count, psTemp );
            char szTemp[ 256 ];
            size_t m = 0;
            for( ; m < nVarLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%hd,", psTemp[m] );
                NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            }
            snprintf( szTemp, sizeof(szTemp),  "%hd", psTemp[m] );
            NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            CPLFree(psTemp);
            break;
        }
        case NC_INT:
        {
            int *pnTemp = reinterpret_cast<int *> (
                CPLCalloc( nVarLen, sizeof( int ) ) );
            nc_get_vara_int( nCdfId, nVarId, start, count, pnTemp );
            char szTemp[ 256 ];
            size_t m = 0;
            for( ; m < nVarLen-1; m++) {
                snprintf( szTemp, sizeof(szTemp), "%d,", pnTemp[m] );
                NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            }
            snprintf( szTemp, sizeof(szTemp), "%d", pnTemp[m] );
            NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            CPLFree(pnTemp);
            break;
        }
        case NC_FLOAT:
        {
            float *pfTemp = reinterpret_cast<float *> (
                CPLCalloc( nVarLen, sizeof( float ) ) );
            nc_get_vara_float( nCdfId, nVarId, start, count, pfTemp );
            char szTemp[ 256 ];
            size_t m = 0;
            for( ; m < nVarLen-1; m++) {
                CPLsnprintf( szTemp, sizeof(szTemp), "%.8g,", pfTemp[m] );
                NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            }
            CPLsnprintf( szTemp, sizeof(szTemp), "%.8g", pfTemp[m] );
            NCDFSafeStrcat(&pszVarValue,szTemp, &nVarValueSize);
            CPLFree(pfTemp);
            break;
        }
        case NC_DOUBLE:
        {
            double *pdfTemp = reinterpret_cast<double *> (
                CPLCalloc( nVarLen, sizeof(double) ) );
            nc_get_vara_double( nCdfId, nVarId, start, count, pdfTemp );
            char szTemp[ 256 ];
            size_t m = 0;
            for( ; m < nVarLen-1; m++) {
                CPLsnprintf( szTemp, sizeof(szTemp), "%.16g,", pdfTemp[m] );
                NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            }
            CPLsnprintf( szTemp, sizeof(szTemp), "%.16g", pdfTemp[m] );
            NCDFSafeStrcat(&pszVarValue, szTemp, &nVarValueSize);
            CPLFree(pdfTemp);
            break;
        }
        default:
            CPLDebug( "GDAL_netCDF", "NCDFGetVar1D unsupported type %d",
                      nVarType );
            CPLFree( pszVarValue );
            pszVarValue = NULL;
            break;
    }

    if ( nVarLen > 1  && nVarType!= NC_CHAR )
        NCDFSafeStrcat(&pszVarValue, "}", &nVarValueSize);

    /* set return values */
    *pszValue = pszVarValue;

    return CE_None;
}

static CPLErr NCDFPut1DVar( int nCdfId, int nVarId, const char *pszValue )
{
    if ( EQUAL( pszValue, "" ) )
        return CE_Failure;


    /* get var information */
    int nVarDimId = -1;
    int status = nc_inq_varndims( nCdfId, nVarId, &nVarDimId );
    if ( status != NC_NOERR || nVarDimId != 1)
        return CE_Failure;

    status = nc_inq_vardimid( nCdfId, nVarId, &nVarDimId );
    if ( status != NC_NOERR )
        return CE_Failure;

    nc_type nVarType = NC_CHAR;
    status = nc_inq_vartype( nCdfId, nVarId, &nVarType );
    if ( status != NC_NOERR )
        return CE_Failure;

    size_t  nVarLen = 0;
    status = nc_inq_dimlen( nCdfId, nVarDimId, &nVarLen );
    if ( status != NC_NOERR )
        return CE_Failure;

    size_t start[1] = {0};
    size_t count[1] = {nVarLen};

    /* get the values as tokens */
    char **papszValues = NCDFTokenizeArray( pszValue );
    if ( papszValues == NULL )
        return CE_Failure;

    nVarLen = CSLCount(papszValues);

    /* now write the data */
    if ( nVarType == NC_CHAR ) {
        status = nc_put_vara_text( nCdfId, nVarId, start, count,
                                  pszValue );
        NCDF_ERR(status);
    }
    else {

        switch( nVarType ) {
            /* TODO add other types */
            case  NC_INT:
            {
                int *pnTemp = reinterpret_cast<int *> (
                    CPLCalloc( nVarLen, sizeof( int ) ) );
                for(size_t i=0; i < nVarLen; i++) {
                    char *pszTemp = NULL;
                    pnTemp[i] = static_cast<int>(strtol( papszValues[i], &pszTemp, 10 ));
                }
                status = nc_put_vara_int( nCdfId, nVarId, start, count, pnTemp );  
                NCDF_ERR(status);
                CPLFree(pnTemp);
                break;
            }
            case  NC_FLOAT:
            {
                float *pfTemp = reinterpret_cast<float *> (
                    CPLCalloc( nVarLen, sizeof( float ) ) );
                for(size_t i=0; i < nVarLen; i++) {
                    char *pszTemp = NULL;
                    pfTemp[i] = (float)CPLStrtod( papszValues[i], &pszTemp );
                }
                status = nc_put_vara_float( nCdfId, nVarId, start, count, 
                                            pfTemp );  
                NCDF_ERR(status);
                CPLFree(pfTemp);
                break;
            }
            case  NC_DOUBLE:
            {
                double *pdfTemp = reinterpret_cast<double *> (
                    CPLCalloc( nVarLen, sizeof( double ) ) );
                for(size_t i=0; i < nVarLen; i++) {
                    char *pszTemp = NULL;
                    pdfTemp[i] = CPLStrtod( papszValues[i], &pszTemp );
                }
                status = nc_put_vara_double( nCdfId, nVarId, start, count, 
                                             pdfTemp );
                NCDF_ERR(status);
                CPLFree(pdfTemp);
                break;
            }
        default:
            if ( papszValues ) CSLDestroy( papszValues );
            return CE_Failure;
            break;
        }
    }

    if ( papszValues )
        CSLDestroy( papszValues );

    return CE_None;
}


/************************************************************************/
/*                           GetDefaultNoDataValue()                    */
/************************************************************************/

double NCDFGetDefaultNoDataValue( int nVarType )

{
    double dfNoData = 0.0;

    switch( nVarType ) {
        case NC_BYTE:
#ifdef NETCDF_HAS_NC4
        case NC_UBYTE:
#endif
            /* don't do default fill-values for bytes, too risky */
            dfNoData = 0.0;
            break;
        case NC_CHAR:
            dfNoData = NC_FILL_CHAR;
            break;
        case NC_SHORT:
            dfNoData = NC_FILL_SHORT;
            break;
        case NC_INT:
            dfNoData = NC_FILL_INT;
            break;
        case NC_FLOAT:
            dfNoData = NC_FILL_FLOAT;
            break;
        case NC_DOUBLE:
            dfNoData = NC_FILL_DOUBLE;
            break;
        default:
            dfNoData = 0.0;
            break;
    }

    return dfNoData;
}


static int NCDFDoesVarContainAttribVal( int nCdfId,
                                 const char * const* papszAttribNames, 
                                 const char * const* papszAttribValues,
                                 int nVarId,
                                 const char * pszVarName,
                                 bool bStrict=true )
{
    if ( (nVarId == -1) && (pszVarName != NULL) )
        nc_inq_varid( nCdfId, pszVarName, &nVarId );

    if ( nVarId == -1 ) return -1;

    bool bFound = false;
    for( int i=0; !bFound && i<CSLCount((char**)papszAttribNames); i++ ) {
        char *pszTemp = NULL;
        if ( NCDFGetAttr( nCdfId, nVarId, papszAttribNames[i], &pszTemp ) 
             == CE_None && pszTemp != NULL ) { 
            if ( bStrict ) {
                if ( EQUAL( pszTemp, papszAttribValues[i] ) )
                    bFound = true;
            }
            else {
                if ( EQUALN( pszTemp, papszAttribValues[i], strlen(papszAttribValues[i]) ) )
                    bFound = true;
            }
            CPLFree( pszTemp );
        }
    }
    return bFound;
}

static int NCDFDoesVarContainAttribVal2( int nCdfId,
                                  const char * papszAttribName, 
                                  const char * const* papszAttribValues,
                                  int nVarId,
                                  const char * pszVarName,
                                  int bStrict=true )
{
    if ( (nVarId == -1) && (pszVarName != NULL) )
        nc_inq_varid( nCdfId, pszVarName, &nVarId );

    if ( nVarId == -1 ) return -1;

    bool bFound = false;
    char *pszTemp = NULL;
    if ( NCDFGetAttr( nCdfId, nVarId, papszAttribName, &pszTemp ) 
         != CE_None || pszTemp == NULL ) return FALSE;

    for( int i=0; !bFound && i<CSLCount((char**)papszAttribValues); i++ ) {
        if ( bStrict ) {
            if ( EQUAL( pszTemp, papszAttribValues[i] ) )
                bFound = true;
        }
        else {
            if ( EQUALN( pszTemp, papszAttribValues[i], strlen(papszAttribValues[i]) ) )
                bFound = true;
        }
    }

    CPLFree( pszTemp );

    return bFound;
}

static bool NCDFEqual( const char * papszName, const char * const* papszValues )
{
    if ( papszName == NULL || EQUAL(papszName,"") )
        return false;

    for( int i=0; i<CSLCount((char**)papszValues); ++i ) {
        if( EQUAL( papszName, papszValues[i] ) )
            return true;
    }

    return false;
}

/* test that a variable is longitude/latitude coordinate, following CF 4.1 and 4.2 */
static bool NCDFIsVarLongitude( int nCdfId, int nVarId,
                        const char * pszVarName )
{
    /* check for matching attributes */
    int bVal = NCDFDoesVarContainAttribVal( nCdfId,
                                            papszCFLongitudeAttribNames,
                                            papszCFLongitudeAttribValues,
                                            nVarId, pszVarName );
    /* if not found using attributes then check using var name */
    /* unless GDAL_NETCDF_VERIFY_DIMS=STRICT */
    if ( bVal == -1 ) {
        if ( ! EQUAL( CPLGetConfigOption( "GDAL_NETCDF_VERIFY_DIMS", "YES" ),
                      "STRICT" ) )
            bVal = NCDFEqual( pszVarName, papszCFLongitudeVarNames );
        else
            bVal = FALSE;
    }
    return CPL_TO_BOOL(bVal);
}

static bool NCDFIsVarLatitude( int nCdfId, int nVarId, const char * pszVarName )
{
    int bVal = NCDFDoesVarContainAttribVal( nCdfId,
                                            papszCFLatitudeAttribNames,
                                            papszCFLatitudeAttribValues,
                                            nVarId, pszVarName );
    if ( bVal == -1 ) {
        if ( ! EQUAL( CPLGetConfigOption( "GDAL_NETCDF_VERIFY_DIMS", "YES" ),
                      "STRICT" ) )
            bVal = NCDFEqual( pszVarName, papszCFLatitudeVarNames );
        else
            bVal = FALSE;
    }
    return CPL_TO_BOOL(bVal);
}

static bool NCDFIsVarProjectionX( int nCdfId, int nVarId, const char * pszVarName )
{
    int bVal = NCDFDoesVarContainAttribVal( nCdfId,
                                            papszCFProjectionXAttribNames,
                                            papszCFProjectionXAttribValues,
                                            nVarId, pszVarName );
    if ( bVal == -1 ) {
        if ( ! EQUAL( CPLGetConfigOption( "GDAL_NETCDF_VERIFY_DIMS", "YES" ),
                      "STRICT" ) )
            bVal = NCDFEqual( pszVarName, papszCFProjectionXVarNames );
        else
            bVal = FALSE;

    }
    return CPL_TO_BOOL(bVal);
}

static bool NCDFIsVarProjectionY( int nCdfId, int nVarId, const char * pszVarName )
{
    int bVal = NCDFDoesVarContainAttribVal( nCdfId,
                                            papszCFProjectionYAttribNames,
                                            papszCFProjectionYAttribValues,
                                            nVarId, pszVarName );
    if ( bVal == -1 ) {
        if ( ! EQUAL( CPLGetConfigOption( "GDAL_NETCDF_VERIFY_DIMS", "YES" ),
                      "STRICT" ) )
            bVal = NCDFEqual( pszVarName, papszCFProjectionYVarNames );
        else
            bVal = FALSE;
    }
    return CPL_TO_BOOL(bVal);
}

/* test that a variable is a vertical coordinate, following CF 4.3 */
static bool NCDFIsVarVerticalCoord( int nCdfId, int nVarId,
                            const char * pszVarName )
{
    /* check for matching attributes */ 
    if ( NCDFDoesVarContainAttribVal( nCdfId,
                                      papszCFVerticalAttribNames,
                                      papszCFVerticalAttribValues,
                                      nVarId, pszVarName ) )
        return true;
    /* check for matching units */ 
    else if ( NCDFDoesVarContainAttribVal2( nCdfId,
                                            CF_UNITS, 
                                            papszCFVerticalUnitsValues,
                                            nVarId, pszVarName ) )
        return true;
    /* check for matching standard name */ 
    else if ( NCDFDoesVarContainAttribVal2( nCdfId,
                                            CF_STD_NAME, 
                                            papszCFVerticalStandardNameValues,
                                            nVarId, pszVarName ) )
        return true;
    else 
        return false;
}

/* test that a variable is a time coordinate, following CF 4.4 */
static bool NCDFIsVarTimeCoord( int nCdfId, int nVarId,
                        const char * pszVarName )
{
    /* check for matching attributes */ 
    if ( NCDFDoesVarContainAttribVal( nCdfId,
                                      papszCFTimeAttribNames, 
                                      papszCFTimeAttribValues,
                                      nVarId, pszVarName ) )
        return true;
    /* check for matching units */ 
    else if ( NCDFDoesVarContainAttribVal2( nCdfId,
                                            CF_UNITS, 
                                            papszCFTimeUnitsValues,
                                            nVarId, pszVarName, false ) )
        return true;
    else
        return false;
}

/* parse a string, and return as a string list */
/* if it an array of the form {a,b} then tokenize it */
/* else return a copy */
static char **NCDFTokenizeArray( const char *pszValue )
{
    if ( pszValue==NULL || EQUAL( pszValue, "" ) )
        return NULL;

    char **papszValues = NULL;
    const int nLen = static_cast<int>(strlen(pszValue));

    if ( pszValue[0] == '{' && nLen > 2 && pszValue[nLen-1] == '}' ) {
        char *pszTemp = reinterpret_cast<char *> (CPLMalloc( (nLen-2) + 1 ) );
        strncpy( pszTemp, pszValue+1, nLen-2);
        pszTemp[nLen-2] = '\0';
        papszValues = CSLTokenizeString2( pszTemp, ",", CSLT_ALLOWEMPTYTOKENS );
        CPLFree( pszTemp );
    }
    else {
        papszValues = reinterpret_cast<char**> (
            CPLCalloc( 2, sizeof(char*) ) );
        papszValues[0] = CPLStrdup( pszValue );
        papszValues[1] = NULL;
    }

    return papszValues;
}
