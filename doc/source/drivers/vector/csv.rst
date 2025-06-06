.. _vector.csv:

Comma Separated Value (.csv)
============================

.. shortname:: CSV

.. built_in_by_default::

OGR supports reading and writing primarily non-spatial tabular data
stored in text CSV files. CSV files are a common interchange format
between software packages supporting tabular data and are also easily
produced manually with a text editor or with end-user written scripts or
programs.

The datasource name may be either a single CSV file or
point to a directory. For a directory to be recognised as a .csv
datasource at least half the files in the directory need to have the
extension .csv. One layer (table) is produced from each .csv file
accessed.
Starting with GDAL 3.7, pipe separated values files with a ".psv" extension
are also recognized.

For files structured as CSV, but not ending
with the ".csv" extension, the 'CSV:' prefix can be added before the filename
to force loading by the CSV driver.

Starting with GDAL 3.10, specifying the ``-if CSV`` option to command line utilities
accepting it, or ``CSV`` as the only value of the ``papszAllowedDrivers`` of
:cpp:func:`GDALOpenEx`, also forces the driver to recognize the passed
filename, without the ``CSV:`` prefix.

The OGR CSV driver supports reading and writing. Because the CSV format
has variable length text lines, reading is done sequentially. Reading
features in random order will generally be very slow. OGR CSV layer
might have a coordinate system stored in a .prj file (see GeoCSV
specification). When reading a field named "WKT" is assumed to contain
WKT geometry, but also is treated as a regular field. The OGR CSV driver
returns all attribute columns as string data types if no field type
information file (with .csvt extension) is available.

Limited type recognition can be done for Integer, Real, String, Date
(YYYY-MM-DD), Time (HH:MM:SS+nn), DateTime (YYYY-MM-DD HH:MM:SS+nn)
columns through a descriptive file with the same name as the CSV file,
but a .csvt extension. In a single line the types for each column have
to be listed with double quotes and be comma separated (e.g.,
"Integer","String"). It is also possible to specify explicitly the width
and precision of each column, e.g.
"Integer(5)","Real(10.7)","String(15)". The driver will then use these
types as specified for the csv columns. Subtypes
can be passed between parenthesis, such as "Integer(Boolean)",
"Integer(Int16)" and "Real(Float32)". Starting with GDAL 2.1,
accordingly with the `GeoCSV
specification <http://giswiki.hsr.ch/GeoCSV>`__, the "CoordX" or
"Point(X)" type can be used to specify a column with longitude/easting
values, "CoordY" or "Point(Y)" for latitude/northing values and "WKT"
for geometries encoded in WKT

Starting with GDAL 2.2, the "JSonStringList", "JSonIntegerList",
"JSonInteger64List" and "JSonRealList" types can be used in .csvt to map
to the corresponding OGR StringList, IntegerList, Integer64List and
RealList types. The field values are then encoded as JSON arrays, with
proper CSV escaping.

Automatic field type guessing can also be done
if specifying the open options described in the below "Open options"
section.

Driver capabilities
-------------------

.. supports_create::

.. supports_georeferencing::

.. supports_virtualio::

Format
------

CSV files have one line for each feature (record) in the layer (table).
The attribute field values are separated by commas. At least two fields
per line must be present. Lines may be terminated by a DOS (CR/LF) or
Unix (LF) style line terminators. Each record should have the same
number of fields. The driver will also accept a semicolon, a tabulation,
a pipe, or a space character as field separator.
Starting with GDAL 3.8, the autodetection will select the separator with the
most occurrences if there are several candidates  on the first line of the CSV
file (and warn about that). The :oo:`SEPARATOR` open option may also be set to
define the desired separator.
Previous versions select comma by default when there are several potential
separators.

Complex attribute values (such as those containing commas, quotes or
newlines) may be placed in double quotes. Any occurrences of double
quotes within the quoted string should be doubled up to "escape" them.

By default, the driver attempts to treat the first line of the file as a
list of field names for all the fields. However, if one or more of the
names is all numeric it is assumed that the first line is actually data
values and dummy field names are generated internally (field_1 through
field_n) and the first record is treated as a feature.
Numeric values are treated as field names if they are
enclosed in double quotes. Starting with GDAL 2.1, this behavior can be
modified via the HEADERS open option.

All CSV files are treated as UTF-8 encoded. A
Byte Order Mark (BOM) at the beginning of the file will be parsed
correctly. The layer creation option :lco:`WRITE_BOM` can be used to create a file
with a Byte Order Mark, which can improve compatibility with some
software (particularly Excel).

Example (employee.csv):

::

   ID,Salary,Name,Comments
   131,11000.0,Jane Lake,Chief Technical Officer
   132,55000.0,John Walker,"The ""big"" cheese."

Note that the Comments value for the first data record is placed in
double quotes because the value contains quotes, and those quotes have
to be doubled up so we know we haven't reached the end of the quoted
string yet.

Many variations of textual input are sometimes called Comma Separated
Value files, including files without commas, but fixed column widths,
those using tabs as separators or those with other auxiliary data
defining field types or structure. This driver does not attempt to
support all such files, but instead to support simple .csv files that
can be auto-recognised. Scripts or other mechanisms can generally be
used to convert other variations into a form that is compatible with the
OGR CSV driver.

Reading CSV containing spatial information
------------------------------------------

Building point geometries
~~~~~~~~~~~~~~~~~~~~~~~~~

Consider the following CSV file (test.csv):

::

   Latitude,Longitude,Name
   48.1,0.25,"First point"
   49.2,1.1,"Second point"
   47.5,0.75,"Third point"

Starting with GDAL 2.1, it is possible to directly specify the potential
names of the columns that can contain X/longitude and Y/latitude with
the :oo:`X_POSSIBLE_NAMES` and :oo:`Y_POSSIBLE_NAMES` open option.

*ogrinfo -ro -al test.csv -oo X_POSSIBLE_NAMES=Lon\* -oo
Y_POSSIBLE_NAMES=Lat\* -oo KEEP_GEOM_COLUMNS=NO* will return :

::

   OGRFeature(test):1
     Name (String) = First point
     POINT (0.25 48.1)

   OGRFeature(test):2
     Name (String) = Second point
     POINT (1.1 49.2)

   OGRFeature(test):3
     Name (String) = Third point
     POINT (0.75 47.5)

If the CSV file does not have a header line, the dummy "field_n" names can be
used as possible names for coordinate fields. For example plain XYZ point
data can be opened as

*ogrinfo -ro -al elevation.xyz -oo X_POSSIBLE_NAMES=field_1 -oo
Y_POSSIBLE_NAMES=field_2 -oo Z_POSSIBLE_NAMES=field_3*

Otherwise, if one or several columns contain a geometry definition
encoded as WKT, WKB (encoded in hexadecimal) or GeoJSON (in which case
the GeoJSON content must be formatted to follow CSV rules, that is to
say it must be surrounded by double-quotes, and double-quotes inside the
string must be repeated for proper escaping), the name of such column(s)
can be derived from the :oo:`GEOM_POSSIBLE_NAMES` open option.

For older versions, it is possible to extract spatial information
(points) from a CSV file which has columns for the X and Y coordinates,
through the use of the :ref:`VRT <vector.vrt>` driver.

You can write the associated VRT file (test.vrt):

::

   <OGRVRTDataSource>
       <OGRVRTLayer name="test">
           <SrcDataSource>test.csv</SrcDataSource>
           <GeometryType>wkbPoint</GeometryType>
           <LayerSRS>WGS84</LayerSRS>
           <GeometryField encoding="PointFromColumns" x="Longitude" y="Latitude"/>
       </OGRVRTLayer>
   </OGRVRTDataSource>

and *ogrinfo -ro -al test.vrt* will return :

::

   OGRFeature(test):1
     Latitude (String) = 48.1
     Longitude (String) = 0.25
     Name (String) = First point
     POINT (0.25 48.1 0)

   OGRFeature(test):2
     Latitude (String) = 49.2
     Longitude (String) = 1.1
     Name (String) = Second point
     POINT (1.1 49.200000000000003 0)

   OGRFeature(test):3
     Latitude (String) = 47.5
     Longitude (String) = 0.75
     Name (String) = Third point
     POINT (0.75 47.5 0)

Building line geometries
~~~~~~~~~~~~~~~~~~~~~~~~

Consider the following CSV file (test.csv):

::

   way_id,pt_id,x,y
   1,1,2,49
   1,2,3,50
   2,1,-2,49
   2,2,-3,50

With a GDAL build with Spatialite enabled, *ogrinfo test.csv -dialect
SQLite -sql "SELECT way_id, MakeLine(MakePoint(CAST(x AS float),CAST(y
AS float))) FROM test GROUP BY way_id"* will return :

::

   OGRFeature(SELECT):0
     way_id (String) = 1
     LINESTRING (2 49,3 50)

   OGRFeature(SELECT):1
     way_id (String) = 2
     LINESTRING (-2 49,-3 50)

VSI Virtual File System API support
-----------------------------------

The driver supports reading and writing to files managed by VSI Virtual
File System API, which include "regular" files, as well as files in the
/vsizip/ (read-write) , /vsigzip/ (read-only) , /vsicurl/ (read-only)
domains.

Writing to /dev/stdout or /vsistdout/ is also supported.

Reading from /vsistdin/ is supported using the ``CSV:/vsistdin/`` connection
string and provided that none of the open options whose name starts with ``AUTODETECT_``
is used.

Open options
------------

|about-open-options|
The following open options are supported:

-  .. oo:: SEPARATOR
      :choices: AUTO, COMMA, SEMICOLON, TAB, SPACE, PIPE
      :default: AUTO
      :since: 3.8

      Field separator character. Default value is AUTO for autodetection.

-  .. oo:: MERGE_SEPARATOR
      :choices: YES, NO
      :default: NO

      Setting it to YES will
      enable merging consecutive separators. Mostly useful when it is the
      space character.

-  .. oo:: AUTODETECT_TYPE
      :choices: YES, NO
      :default: NO

      Setting it to YES will
      enable auto-detection of field data types. If while reading the
      records (beyond the records used for autodetection), a value is found
      to not correspond to the autodetected data type, a warning will be
      emitted and the field will be emptied.

-  .. oo:: KEEP_SOURCE_COLUMNS
      :choices: YES, NO
      :default: NO

      keep a copy of the
      original columns where the guessing is active, and the guessed type
      is different from string. The name of the original columns will be
      suffixed with "_original". This flag should be used only when
      ..oo::`AUTODETECT_TYPE=YES`.

-  .. oo:: AUTODETECT_WIDTH
      :choices: YES, NO, STRING_ONLY
      :default: NO

      Setting
      it to YES to detect the width of string and integer fields, and the
      width and precision of real fields. Setting it to STRING_ONLY
      restricts to string fields. Setting it to NO select default size and
      width. If while reading the records (beyond the records used for
      autodetection), a value is found to not correspond to the
      autodetected width/precision, a warning will be emitted and the field
      will be emptied.

-  .. oo:: AUTODETECT_SIZE_LIMIT
      :choices: <bytes>
      :default: 1000000

      size to specify the number of bytes to
      inspect to determine the data type and width/precision. The default
      will be 1 000 000. Setting 0 means inspecting the whole file. Note:
      when reading from standard input, this will be limited to 1 MB, due to
      how /vsistdin/ is implemented..

-  .. oo:: QUOTED_FIELDS_AS_STRING
      :choices: YES, NO
      :default: NO

      Only used if
      :oo:`AUTODETECT_TYPE=YES`. Whether to enforce quoted fields as string
      fields when set to YES. Otherwise, by default, the content of quoted
      fields will be tested for real, integer, etc... data types.

-  .. oo:: X_POSSIBLE_NAMES
      :choices: <list_of_names>
      :since: 2.1

      Comma separated
      list of possible names for X/longitude coordinate of a point. Each
      name might be a pattern using the star character in starting and/or
      ending position. E.g.: prefix*, \*suffix or \*middle*. The values in
      the column must be floating point values. :oo:`X_POSSIBLE_NAMES` and
      Y_POSSIBLE_NAMES must be both specified and a matching for each must
      be found in the columns of the CSV file. Only one geometry column per
      layer might be built when using :oo:`X_POSSIBLE_NAMES`/:oo:`Y_POSSIBLE_NAMES`.

-  .. oo:: Y_POSSIBLE_NAMES
      :choices: <list_of_names>
      :since: 2.1

      Comma separated
      list of possible names for Y/latitude coordinate of a point. Each
      name might be a pattern using the star character in starting and/or
      ending position. E.g.: prefix*, \*suffix or \*middle*. The values in
      the column must be floating point values. :oo:`X_POSSIBLE_NAMES` and
      :oo:`Y_POSSIBLE_NAMES` must be both specified and a matching for each must
      be found in the columns of the CSV file.

-  .. oo:: Z_POSSIBLE_NAMES
      :choices: <list_of_names>
      :since: 2.1

      Comma separated
      list of possible names for Z/elevation coordinate of a point. Each
      name might be a pattern using the star character in starting and/or
      ending position. E.g.: prefix*, \*suffix or \*middle*. The values in
      the column must be floating point values. Only taken into account in
      combination with :oo:`X_POSSIBLE_NAMES` and :oo:`Y_POSSIBLE_NAMES`.

-  .. oo:: GEOM_POSSIBLE_NAMES
      :choices: <list_of_names>
      :since: 2.1

      Comma
      separated list of possible names for geometry columns that contain
      geometry definitions encoded as WKT, WKB (in hexadecimal form,
      potentially in PostGIS 2.0 extended WKB) or GeoJSON. Each name might
      be a pattern using the star character in starting and/or ending
      position. E.g.: prefix*, \*suffix or \*middle\*

-  .. oo:: KEEP_GEOM_COLUMNS
      :choices: YES, NO
      :default: YES

      Expose the detected
      X,Y,Z or geometry columns as regular attribute fields.

-  .. oo:: HEADERS
      :choices: YES, NO, AUTO
      :default: AUTO
      :since: 2.1

      Whether the
      first line of the file contains column names or not. When set to
      AUTO, GDAL will assume the first line is column names if none of the
      values are strictly numeric.

-  .. oo:: EMPTY_STRING_AS_NULL
      :choices: YES, NO
      :default: NO
      :since: 2.1

      Whether to consider empty strings as null fields on reading'.

-  .. oo:: MAX_LINE_SIZE
      :choices: <integer>
      :default: 10000000
      :since: 3.5.3

      Maximum number of bytes for a line (-1=unlimited).

-  .. oo:: OGR_SCHEMA
      :choices: <filename>|<json string>
      :since: 3.11.0

      Partially or totally overrides the auto-detected schema to use for creating the layer.
      The overrides are defined as a JSON list of field definitions.
      This can be a filename, a URL or JSON string conformant with the `ogr_fields_override.schema.json schema <https://raw.githubusercontent.com/OSGeo/gdal/refs/heads/master/ogr/data/ogr_fields_override.schema.json>`_
      This option takes precedence over any other option and over the .csvt file.


Creation Issues
---------------

The driver supports creating new databases (as a directory of .csv
files), adding new .csv files to an existing directory or .csv files or
appending features to an existing .csv table. Starting with GDAL 2.1,
deleting or replacing existing features, or adding/modifying/deleting
fields is supported, provided the modifications done are small enough to
be stored in RAM temporarily before flushing to disk.

Layer Creation options
----------------------

|about-layer-creation-options|
The following layer creation options are supported:

-  .. lco:: LINEFORMAT
      :choices: CRLF, LF

      By default when creating new .csv files they are
      created with the line termination conventions of the local platform
      (CR/LF on win32 or LF on all other systems). This may be overridden
      through use of the :lco:`LINEFORMAT` layer creation option which may have a
      value of **CRLF** (DOS format) or **LF** (Unix format).

-  .. lco:: GEOMETRY
      :choices: AS_WKT, AS_XYZ, AS_XY, AS_YZ

      By default, the geometry of
      a feature written to a .csv file is discarded. It is possible to
      export the geometry in its WKT representation by specifying
      GEOMETRY=\ **AS_WKT**. It is also possible to export point geometries
      into their X,Y,Z components (different columns in the csv file) by
      specifying GEOMETRY=\ **AS_XYZ**, GEOMETRY=\ **AS_XY** or
      GEOMETRY=\ **AS_YX**. The geometry column(s) will be prepended to the
      columns with the attributes values. It is also possible to export
      geometries in GeoJSON representation using SQLite SQL dialect query,
      see example below.

-  .. lco:: CREATE_CSVT
      :choices: YES, NO
      :default: NO

      Create the
      associated .csvt file (see above paragraph) to describe the type of
      each column of the layer and its optional width and precision.
      This option also creates .prj file which stores coordinate system information.

-  .. lco:: SEPARATOR
      :choices: COMMA, SEMICOLON, TAB, SPACE, PIPE
      :default: COMMA

      Field separator character.

-  .. lco:: WRITE_BOM
      :choices: YES, NO
      :default: NO

      Write a UTF-8 Byte Order Mark (BOM) at the start of the file.

-  .. lco:: GEOMETRY_NAME
      :since: 2.1
      :default: WKT

      Name of geometry column. Only used if :lco:`GEOMETRY=AS_WKT` (and
      :lco:`CREATE_CSVT=YES` before GDAL 3.7.1).

-  .. lco:: STRING_QUOTING
      :choices: IF_NEEDED, IF_AMBIGUOUS, ALWAYS
      :since: 2.3
      :default: IF_AMBIGUOUS

      whether to double-quote strings. IF_AMBIGUOUS means that
      string values that look like numbers will be quoted (it also implies
      IF_NEEDED). Defaults to IF_AMBIGUOUS (behavior in older versions was
      IF_NEEDED)

Configuration options
---------------------

|about-config-options|
The following configuration options are available:

-  .. config:: OGR_WKT_PRECISION
      :choices: <integer>
      :default: 15

      Number of decimals for coordinate
      values. A heuristic is used to remove insignificant
      trailing 00000x or 99999x that can appear when formatting decimal
      numbers. Examples: 6 gives 120.864, 24.1818; 2 gives 1.2E+02, 24.0.

-  .. config:: OGR_WKT_ROUND
      :choices: YES, NO
      :default: YES
      :since: 2.3

      Whether to enable the above
      mentioned heuristics to remove insignificant trailing 00000x or
      99999x.

Examples
~~~~~~~~

-  This example shows using ogr2ogr to transform a shapefile with point
   geometry into a .csv file with the X,Y,Z coordinates of the points as
   first columns in the .csv file

   ::

      ogr2ogr -f CSV output.csv input.shp -lco GEOMETRY=AS_XYZ

-  This example shows using ogr2ogr to transform a shapefile into a .csv
   file with geometry field formatted using GeoJSON format.

   ::

      ogr2ogr -f CSV output.csv input.shp -dialect sqlite -sql \
          "select AsGeoJSON(geometry) AS geom, * from input"

- Convert a CSV into a GeoPackage. Specify the names of the coordinate columns and assign a coordinate reference system.

   ::

     ogr2ogr \
       -f GPKG output.gpkg \
       input.csv \
       -oo X_POSSIBLE_NAMES=longitude \
       -oo Y_POSSIBLE_NAMES=latitude \
       -a_srs 'EPSG:4326'

-  Use `ogr2ogr -segmentize` to densify a input geometry being specified in the ``WKT`` special field. Note that one needs to specify the GEOMETRY=AS_WKT layer creation option, otherwise the input geometry would be returned unmodified:

   ::

    $ cat input.csv
    WKT,ID,Name
    "LINESTRING (-900 -1450,-900 100)",0,900W

    $ ogr2ogr -segmentize 400 -lco GEOMETRY=AS_WKT \
      -sql "SELECT ID, Name FROM input" output.csv input.csv

    $ cat output.csv
    WKT,ID,Name
    "LINESTRING (-900 -1450,-900 -1062.5,-900 -675,-900 -287.5,-900 100)","0",900W


Particular datasources
----------------------

The CSV driver can also read files whose structure is close to CSV files
:

-  Airport data files NfdcFacilities.xls, NfdcRunways.xls,
   NfdcRemarks.xls and NfdcSchedules.xls found on that `FAA
   website <http://www.faa.gov/airports/airport_safety/airportdata_5010/menu/index.cfm>`__

-  Files from the `USGS
   GNIS <http://geonames.usgs.gov/domestic/download_data.htm>`__
   (Geographic Names Information System)

-  The allCountries file from `GeoNames <http://www.geonames.org>`__

-  `Eurostat .TSV
   files <http://epp.eurostat.ec.europa.eu/NavTree_prod/everybody/BulkDownloadListing?file=read_me.pdf>`__

Other Notes
-----------

-  `GeoCSV specification <http://giswiki.hsr.ch/GeoCSV>`__ (supported by
   GDAL >= 2.1)
-  Initial development of the OGR CSV driver was supported by `DM
   Solutions Group <http://www.dmsolutions.ca/>`__ and
   `GoMOOS <http://www.gomoos.org/>`__.
-  `Carto <https://carto.com/>`__ funded field type auto-detection and
   open options related to geometry columns.
