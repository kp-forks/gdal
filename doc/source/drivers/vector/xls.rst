.. _vector.xls:

XLS - MS Excel format
=====================

.. shortname:: XLS

.. build_dependencies:: libfreexl

This driver reads spreadsheets in the (old) Microsoft Excel binary format. This
format pre-dates Excel Open XML Spreadsheet, introduced in Microsoft Excel 2007,
and supported by the :ref:`XLSX <vector.xlsx>` driver.

GDAL/OGR must be built against the FreeXL library (GPL/LPL/MPL licensed), and the driver
has the same restrictions as the FreeXL library itself as far as which
and how Excel files are supported, in particular formulas are not supported.

Each sheet is presented as a OGR layer. No geometry support is available
directly (but you may use the OGR VRT capabilities for that).

Configuration options
---------------------

|about-config-options|
The following configuration options are available:

-  .. config:: OGR_XLS_HEADERS
      :choices: FORCE, DISABLE, AUTO
      :default: AUTO

      By default, the driver
      will read the first lines of each sheet to detect if the first line
      might be the name of columns. If set to FORCE, the driver will
      consider the first line will be taken as the header line. If set to
      DISABLE, it will be considered as the first feature. Otherwise
      auto-detection will occur.

-  .. config:: OGR_XLS_FIELD_TYPES
      :choices: STRING, AUTO
      :default: AUTO

      By default, the driver will try
      to detect the data type of fields. If set to STRING, all fields will
      be of String type.

See Also
--------

-  `Homepage of the FreeXL
   library <https://www.gaia-gis.it/fossil/freexl/index>`__
