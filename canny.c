#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>
#include <math.h>
#include <string.h>

//================================================================================ COUNTERS FOR MEMORY DEBUGING
gint set_allocations = 0;
gint set_frees = 0;
gint grid_allocations = 0;
gint grid_frees = 0;
//================================================================================ FUNCTION DECLARATIONS
static void query       (void);
static void run         (const gchar      *name,
                         gint              nparams,
                         const GimpParam  *param,
                         gint             *nreturn_vals,
                         GimpParam       **return_vals);
static gboolean canny_dialog (GimpDrawable *drawable);
static void canny_detection ( GimpDrawable * drawable, GimpPreview * preview );

//================================================================================ PARAMETERS
struct params
{
    gint preview;
    gint high_threshold;
    gint low_threshold;
};
struct params thresholds = {
    1,
    80,
    40
};
GimpPlugInInfo PLUG_IN_INFO =
{
  NULL,  //init
  NULL,  //quit
  query,
  run
};

MAIN()
//================================================================================ QUERY
static void query (void)
{
  static GimpParamDef args[] =
  {
    {
      GIMP_PDB_INT32,
      "run-mode",
      "Run mode"
    },
    {
      GIMP_PDB_IMAGE,
      "image",
      "Input image"
    },
    {
      GIMP_PDB_DRAWABLE,
      "drawable",
      "Input drawable"
    }
  };

  gimp_install_procedure (
    "canny-edge-detection",
    "Detection of edges in picture using the Canny edge detector",
    "Module finds and highlights edges in picture",
    "David Mladek",
    "mladedav",
    "2017",
    "C_anny edge detection", //TODO change name to "_Canny..." when changing menu placement
    "RGB, GRAY, INDEXED", // TODO maybe add handeling of alpha channels later and indexed
    GIMP_PLUGIN,
    G_N_ELEMENTS (args), 0,
    args, NULL);

//TODO change plugin menu placement to "<Image>/Filters/Edge-Detection"
  gimp_plugin_menu_register ("canny-edge-detection","<Image>");
}
//================================================================================ RUN
static void
      run (const gchar      *name,
           gint              nparams,
           const GimpParam  *param,
           gint             *nreturn_vals,
           GimpParam       **return_vals)
{
    static GimpParam  values;
    GimpRunMode       run_mode;
    GimpDrawable     *drawable;

    /* Setting mandatory output values */
    *nreturn_vals = 1;
    *return_vals  = &values;

    values.type = GIMP_PDB_STATUS;

    run_mode = param[0].data.d_int32;

    drawable = gimp_drawable_get (param[2].data.d_drawable);

    if ( !gimp_drawable_is_gray ( drawable -> drawable_id ) )
    {
        // TODO add undo set so this is undoable in one go
        gimp_image_convert_grayscale ( param[1].data.d_int32 );
        drawable -> bpp = 1; // I am scared of this but I had to do this so that it works even with RGB
    }
    if ( !gimp_drawable_is_gray ( drawable -> drawable_id ) )
        g_error ( "Cannot convert drawable to grayscale" );

    switch (run_mode)
    {
        case GIMP_RUN_NONINTERACTIVE:
        if (nparams != 5)
        {
            values . data . d_status = GIMP_PDB_CALLING_ERROR;
            return;
        }
        thresholds . low_threshold = param [ 3 ] . data . d_int32;
        thresholds . high_threshold = param [ 4 ] . data . d_int32;
        case GIMP_RUN_INTERACTIVE:
        if ( !canny_dialog (drawable) )
            g_error ( "Unexpected problem with dialog window." );
        break;
        case GIMP_RUN_WITH_LAST_VALS: // I don't think this is viable option
        break;
        default:
        break;
    }

    gimp_progress_init ("Canny edge detection...");

    g_debug ( "Starting canny_detection()" );
    canny_detection ( drawable, NULL );
    g_debug ( "Finished canny_detection()" );

    values . data . d_status = GIMP_PDB_SUCCESS;

    gimp_displays_flush ();
    gimp_drawable_detach (drawable);
}
//================================================================================ GRID DEFINITIONS
typedef struct grid
{
    gint width;
    gint height;
    gint channels;
    guchar * data;
} GRID;

static GRID * grid_init ( gint width, gint height, gint channels )
{
    grid_allocations ++;
    GRID * ret = g_new ( GRID, 1 );
    ret -> width = width;
    ret -> height = height;
    ret -> channels = channels;
    ret -> data = g_new ( guchar, width * height * channels );
    return ret;
}

static void grid_free ( GRID * grid )
{
    grid_frees ++;
    g_free ( grid -> data );
    g_free ( grid );
}

// MAGIC HAPPENING ON FOLLOWING LINES
// tl;dr getByte from GRID "g" with coordinates "x" and "y" and get the "ch"th channel from that pixel
//
// I wanted to use macro because C does not have references so it helps.
// There are some sanitations for the bounds of the grid which make this look scary.

#define getByte(g, x, y, ch) (g) -> data \
[ ((y)<0 ? -(y) : (y)>=(g)->height ? 2*(g)->height-(y)-1 : (y)) * (g)->width * (g)->channels \
+ ((x)<0 ? -(x) : (x)>=(g)->width  ? 2*(g)->width -(x)-1 : (x)) * (g)->channels \
+ (ch) ]
//================================================================================ MATRIXES USED IN ALGORITHM
static gint gauss_matrix [ 5 ][ 5 ] = { 
    { 2, 4, 5, 4, 2 },
    { 4, 9, 12, 9 , 4 },
    { 5, 12, 15, 12, 5 },
    { 4, 9, 12, 9 , 4 },
    { 2, 4, 5, 4, 2 }
};

static gint sobel_matrix_x [ 3 ][ 3 ] = {
    { 1, 0, -1 },
    { 2, 0, -2 },
    { 1, 0, -1 }
};

static gint sobel_matrix_y [ 3 ][ 3 ] = {
    { 1, 2, 1 },
    { 0, 0, 0 },
    { -1, -2, -1 }
};
//================================================================================ 1ST PART - GAUSSIAN BLUR
static void gauss_smooth ( GRID * in, GRID * out )
{
    gint i, j, k, a, b;

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
            for ( k = 0 ; k < in -> channels ; k ++ )
            {
                gint smooth = 0;
                for ( a = 0 ; a < 5 ; a ++ )
                    for ( b = 0 ; b < 5 ; b ++ )
                        smooth += gauss_matrix [ a ][ b ] * getByte ( in, i + a - 2, j + b - 2, k );
                getByte ( out, i, j, k ) = smooth / 159.0 + 0.5;
            }
}
//================================================================================ 2ND PART - SOBEL INTENSITY GRADIENT
static void sobel_intensity_gradient ( GRID * in, GRID * out )
{
    gint i, j, a, b;

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
        {
            gint gx = 0, gy = 0;
            gdouble direction;
            for ( a = 0 ; a < 3 ; a ++ )
                for ( b = 0 ; b < 3 ; b ++ )
                {
                    gx += sobel_matrix_x [ a ][ b ] * getByte ( in, i + a - 1, j + b - 1, 0 );
                    gy += sobel_matrix_y [ a ][ b ] * getByte ( in, i + a - 1, j + b - 1, 0 );
                }
            getByte ( out, i, j, 0 ) = CLAMP0255 ( hypot ( gx, gy ) );

            direction = atan2 ( gx, gy ) * 180 / M_PI; // fucken magic gx is not gy because fuck me and I don't know what was happening
            if ( direction < 0 )
                direction += 180;
            if ( direction >= 180 )
                direction -= 180;
            if ( direction < 22.5 )
                getByte ( out, i, j, 1 ) = 0;
            else if ( direction < 67.5 )
                getByte ( out, i, j, 1 ) = 45;
            else if ( direction < 112.5 )
                getByte ( out, i, j, 1 ) = 90;
            else if ( direction < 157.5 )
                getByte ( out, i, j, 1 ) = 135;
            else
                getByte ( out, i, j, 1 ) = 0;
        }
}
//================================================================================ 3RD PART - NON MAXIMUM SUPPRESSION
// It's ok after all, but it is a little bit of magic again.
// I want only bytes that are of higher intensity than others or are on edge
// where there are more bytes of the same intensity

#define suppress( in, out, x, y, x2, y2, x3, y3, val ) \
getByte ( (out), (x), (y), 0 ) = \
( (val) < getByte ( (in), (x2), (y2), 0 ) || (val) <= getByte ( (in), (x3), (y3), 0 ) ) && \
( (val) <= getByte ( (in), (x2), (y2), 0 ) || (val) < getByte ( (in), (x3), (y3), 0 ) ) ? 0 : (val)

static void non_maximum_suppression ( GRID * in, GRID * out )
{
    gint i, j;

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
        {
            gint val = getByte ( in, i, j, 0 );
            switch ( getByte ( in, i, j, 1 ) )
            {
              case 0:
                suppress ( in, out, i, j, i - 1, j, i + 1, j, val );
                break;
              case 45:
                suppress ( in, out, i, j, i - 1, j - 1, i + 1, j + 1, val );
                break;
              case 90:
                suppress ( in, out, i, j, i, j - 1, i, j + 1, val );
                break;
              case 135:
                suppress ( in, out, i, j, i - 1, j + 1, i + 1, j - 1, val );
                break;
              default:
                g_error ( "Unexpected value of gradient direction (%d). Discard the output of this operation.", getByte ( in, i, j, 1 ) );
                return;
            }
        }
}
//================================================================================ 4TH PART - DOUBLE THRESHOLD DETECTION
static void double_threshold ( GRID * in, GRID * out )
{
    gint i, j;

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
            if ( getByte ( in, i, j, 0 ) < thresholds . low_threshold )
                getByte ( out, i, j, 0 ) = 0;
            else if ( getByte ( in, i, j, 0 ) < thresholds . high_threshold )
                getByte ( out, i, j, 0 ) = 127;
            else
                getByte ( out, i, j, 0 ) = 255;
}
//================================================================================ HELPING STRUCTURES AND FUNCTIONS
gint lower_bound ( gint * arr, gint size, gint val )
{
    gint lo = 0, hi = size;
    if ( size == 0 )
        return 0;
    while ( lo >= hi )
    {
        gint index = ( hi + lo ) / 2;
        if ( arr [ index ] > val )
            lo = ( hi + lo ) / 2 + 1;
        else if ( arr [ index ] < val )
            hi = ( hi + lo ) / 2 - 1;
        else
            break;
    }
    return lo;
}
//-------------------------------------------------------------------------------- SET
#define SET_DEFAULT_SIZE 32
typedef struct set_gint
{
    gint size;
    gint max;
    gint * data;
} SET;

SET * set_init ( gint max )
{
    set_allocations++;
    SET * ret = g_new ( SET, 1 );
    ret -> size = 0;
    ret -> max = max;
    ret -> data = g_new ( gint, ret -> max );
    return ret;
}

void set_free ( SET * s )
{
    set_frees++;
    g_free ( s -> data );
    g_free ( s );
}

void set_grow ( SET * s )
{
    s -> max *= 2;
    s -> data = g_realloc ( s -> data, s -> max * sizeof ( gint ) );
}

void set_insert ( SET * s, gint val )
{
    gint index = lower_bound ( s -> data, s -> size, val );
    if ( s -> data [ index ] == val )
        return;
    if ( s -> size == s -> max )
        set_grow ( s );
    memmove ( s -> data + index + 1, s -> data + index, ( s -> size - index ) * sizeof ( gint ) );
    s -> data [ index ] = val;
    s -> size ++;
}

SET * set_merge ( SET * first, SET * second )
{
    gint i = 0, j = 0;
    SET * ret;
    gint merged_size = first -> size + second -> size;
    gint merged_max = first -> max > second -> max ? first -> max : second -> max;
    if ( merged_size > merged_max )
        merged_max *= 2;
    ret = set_init ( merged_max );
    ret -> size =  merged_size;

    while ( i + j <= ret -> size )
    {
        if ( i == first -> size )
        {
            ret -> data [ i + j ] = second -> data [ j ];
            j ++;
        }
        else if ( j == second -> size )
        {
            ret -> data [ i + j ] = first -> data [ i ];
            i ++;
        }
        else if ( first -> data [ i ] <= second -> data [ j ] )
        {
            ret -> data [ i + j ] = first -> data [ i ];
            i ++;
        }
        else
        {
            ret -> data [ i + j ] = second -> data [ j ];
            j ++;
        }
    }
    return ret;
}
//-------------------------------------------------------------------------------- LABELS
typedef struct valid_labels
{
    gint size;
    gint max;
    gint * valid;
    SET ** equivalent;
} LABELS;

LABELS * labels_init ( void )
{
    LABELS * ret = g_new ( LABELS, 1 );
    ret -> size = 1;
    ret -> max = 1024;
    ret -> valid = g_new0 ( gint, ret -> max );
    ret -> equivalent = g_new ( SET *, ret -> max );
    return ret;
}

void labels_free ( LABELS * l )
{
    gint i;
    for ( i = 1 ; i < l -> size ; i ++ )
        if ( l -> equivalent [ i ] -> data [ l -> equivalent [ i ] -> size - 1 ] == i )
            set_free ( l -> equivalent [ i ] );
    g_free ( l -> equivalent );
    g_free ( l -> valid );
    g_free ( l );
}

void labels_grow ( LABELS * l )
{
    l -> max *= 2;
    l -> valid = g_realloc ( l -> valid, l -> max * sizeof ( gint ) );
    l -> equivalent = g_realloc ( l -> equivalent, l -> max * sizeof ( SET * ) );
}

gint labels_add ( LABELS * l )
{
    if ( l -> size == l -> max )
        labels_grow ( l );
    l -> equivalent [ l -> size ] = set_init ( SET_DEFAULT_SIZE );
    set_insert ( l -> equivalent [ l -> size ], l -> size );
    return l -> size ++;
}

void labels_merge ( LABELS * labels, gint first, gint second )
{
    gint i;
    SET * first_set = labels -> equivalent [ first ];
    SET * second_set = labels -> equivalent [ second ];
    if ( first_set == second_set )
        return;
    SET * merged = set_merge ( first_set, second_set );

    for ( i = 0 ; i < merged -> size ; i ++ )
        labels -> equivalent [ merged -> data [ i ] ] = merged;

    set_free ( first_set );
    set_free ( second_set );
}

void labels_validate ( LABELS * labels, gint val )
{
    labels -> valid [ val ] = 1;
}

gint labels_is_valid ( LABELS * labels, gint val )
{
    gint i;
    SET * equal = labels -> equivalent [ val ];
    if ( val == 0 )
        return 0;
    if ( labels -> valid [ val ] )
        return 1;
    for ( i = 0 ; i < equal -> size ; i ++ )
        if ( labels -> valid [ equal -> data [ i ] ] )
            return labels -> valid [ val ] = 1;
    return 0;
}
//================================================================================ 5TH PART - EDGE TRACKING BY HYSTERESIS
static void hysteresis ( GRID * in, GRID * out )
{
    gint i, j;
    LABELS * valid = labels_init ();
    gint ** labels = g_new ( gint *, in -> width );
    for ( i = 0 ; i < in -> width ; i ++ )
        labels [ i ] = g_new0 ( gint, in -> height );

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
        {
            gint min = 0;
            gint a, b;
            if ( getByte ( in, i, j, 0 ) == 0 )
                continue;
            for ( a = 0 ; a < 3 ; a ++ )
            {
                if ( i + a - 1 < 0 || i + a - 1 >= in -> width )
                    continue;
                for ( b = 0 ; b < 3 ; b ++ )
                {
                    gint label;
                    if ( j + b - 1 < 0 || j + b - 1 >= in -> height )
                        continue;
                    label = labels [ i + a - 1 ][ j + b - 1 ];
                    if ( label == 0 )
                        continue;
                    if ( min == 0 )
                        min = label;
                    else
                    {
                        if ( label != min )
                            labels_merge ( valid, label, min );
                        if ( label < min )
                            min = label;
                    }
                }
            }
            if ( min == 0 )
                min = labels_add ( valid );
            labels [ i ][ j ] = min;
            if ( getByte ( in, i, j, 0 ) == 255 )
                labels_validate ( valid, min );
        }

    for ( i = 0 ; i < in -> width ; i ++ )
        for ( j = 0 ; j < in -> height ; j ++ )
            if ( labels_is_valid ( valid, labels [ i ][ j ] ) )
                getByte ( out, i, j, 0 ) = 255;
            else
                getByte ( out, i, j, 0 ) = 0;

    for ( i = 0 ; i < in -> width ; i ++ )
        g_free ( labels [ i ] );
    g_free ( labels );
    labels_free ( valid );
}

static void canny_detection ( GimpDrawable * drawable, GimpPreview * preview )
{
    gint x1, x2, y1, y2, width, height;
    GimpPixelRgn rgn_in;
    GimpPixelRgn rgn_out;
    GRID *in, *blurred, *intensity, *thinned, *threshold, *out;

    set_allocations = 0;
    set_frees = 0;
    grid_allocations = 0;
    grid_frees = 0;

    // This has no effect but provides clarification as to how the double_threshold acts
    if ( thresholds . low_threshold > thresholds . high_threshold )
        thresholds . high_threshold = thresholds . low_threshold;

    g_debug ( "Getting mask bounds" );
    if ( preview )
    {
        gimp_preview_get_position ( preview, &x1, &y1 );
        gimp_preview_get_size ( preview, &width, &height );
        x2 = x1 + width;
        y2 = y2 + height;
    }
    else
    {
        gimp_drawable_mask_bounds ( drawable -> drawable_id, &x1, &y1, &x2, &y2 );
        width = x2 - x1;
        height = y2 - y1;
    }

    g_debug ( "Initializing pixel regions" );
    gimp_pixel_rgn_init (&rgn_in,  drawable, x1, y1, width, height, FALSE, FALSE );
    gimp_pixel_rgn_init (&rgn_out, drawable, x1, y1, width, height, TRUE, TRUE );

    g_debug ( "Allocating grids" );
    in = grid_init ( width, height, 1 );
    blurred = grid_init ( width, height, 1 );
    g_debug ( "Reading data" );
    gimp_pixel_rgn_get_rect ( &rgn_in, in -> data, x1, y1, width, height );

    g_debug ( "Starting gaussian blur" );
    gimp_progress_set_text ( "Gauss smooth..." );
    gauss_smooth ( in, blurred );
    g_debug ( "Finished gaussian blur" );
    grid_free ( in );

    intensity = grid_init ( width, height, 2 ); // TODO maybe use parasites instead of second channel
    g_debug ( "Starting sobel gradient" );
    gimp_progress_set_text ( "Sobel intensity gradient..." );
    sobel_intensity_gradient ( blurred, intensity );
    g_debug ( "Finished sobel gradient" );
    grid_free ( blurred );

    thinned = grid_init ( width, height, 1 );
    g_debug ( "Starting non-maximum suppression" );
    gimp_progress_set_text ( "Non-maximum suppression..." );
    non_maximum_suppression ( intensity, thinned );
    g_debug ( "Finished non-maximum suppression" );
    grid_free ( intensity );

    threshold = grid_init ( width, height, 1 );
    g_debug ( "Starting double threshold" );
    gimp_progress_set_text ( "Double threshold..." );
    double_threshold ( thinned, threshold );
    g_debug ( "Finished double threshold" );
    grid_free ( thinned );

    out = grid_init ( width, height, 1 );
    g_debug ( "Starting edge tracking by hysteresis" );
    gimp_progress_set_text ( "Edge tracking..." );
    hysteresis ( threshold, out );
    g_debug ( "Finished edge tracking by hysteresis" );
    grid_free ( threshold );

    g_debug ( "Writing results" );
    gimp_pixel_rgn_set_rect ( &rgn_out, out -> data, x1, y1, width, height );
    grid_free ( out );

    if ( preview )
    {
        g_debug ( "Updating preview" );
        gimp_drawable_preview_draw_region ( GIMP_DRAWABLE_PREVIEW ( preview ), &rgn_out );
    }
    else
    {
        g_debug ( "Updating drawable" );
        gimp_drawable_flush (drawable);
        gimp_drawable_merge_shadow (drawable->drawable_id, TRUE);
        gimp_drawable_update (drawable->drawable_id, x1, y1, width, height);
    }

    g_debug ( "Set allocations done:  %d", set_allocations );
    g_debug ( "Set frees done:        %d", set_frees );
    g_debug ( "Grid allocations done: %d", grid_allocations );
    g_debug ( "Grid frees done:       %d", grid_frees );
    if ( set_allocations != set_frees || grid_allocations != grid_frees )
        g_warning ( "High possibility memory leaks occured. Saving all work is recommended." );
}
//================================================================================ USER INTERFACE
static gboolean canny_dialog (GimpDrawable *drawable)
{
  GtkWidget *dialog;
  GtkWidget *main_vbox;
  GtkWidget *table;
  GtkWidget *preview;
  GtkWidget *frame;
  GtkWidget *frame_label;
  gboolean   run;

  GtkWidget *label_lo;
  GtkWidget *spinbutton_lo;
  GtkObject *spinbutton_adj_lo;
  GtkWidget *label_hi;
  GtkWidget *spinbutton_hi;
  GtkObject *spinbutton_adj_hi;


  gimp_ui_init ("canny", FALSE);

  dialog = gimp_dialog_new ("Canny edge detection", "canny",
                            NULL, 0,
                            gimp_standard_help_func, "canny_detection",
                            GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                            GTK_STOCK_OK,     GTK_RESPONSE_OK,
                            NULL);

  main_vbox = gtk_vbox_new (FALSE, 6);
  gtk_container_add (GTK_CONTAINER (GTK_DIALOG (dialog)->vbox), main_vbox);
  gtk_widget_show (main_vbox);

  preview = gimp_drawable_preview_new (drawable, &thresholds . preview);
  gtk_box_pack_start (GTK_BOX (main_vbox), preview, TRUE, TRUE, 0);
  gtk_widget_show (preview);

  frame = gtk_frame_new (NULL);
  gtk_widget_show (frame);
  gtk_box_pack_start (GTK_BOX (main_vbox), frame, TRUE, TRUE, 0);
  gtk_container_set_border_width (GTK_CONTAINER (frame), 6);

  table = gtk_table_new (2, 2, TRUE);
  gtk_widget_show (table);
  gtk_container_add (GTK_CONTAINER (frame), table);
//====================================================================================================
  label_lo = gtk_label_new_with_mnemonic ("_Low threshold");
  gtk_widget_show (label_lo);
  gtk_table_attach_defaults (GTK_TABLE(table), label_lo, 0, 1, 0, 1);
  gtk_label_set_justify (GTK_LABEL (label_lo), GTK_JUSTIFY_RIGHT);

  spinbutton_lo = gimp_spin_button_new (&spinbutton_adj_lo, thresholds . low_threshold, 1, 255, 1, 1, 1, 5, 0);
  gtk_table_attach_defaults (GTK_TABLE(table), spinbutton_lo, 1, 2, 0, 1);
  gtk_widget_show (spinbutton_lo);

  g_signal_connect_swapped (spinbutton_adj_lo, "value_changed",
                            G_CALLBACK (gimp_preview_invalidate),
                            preview);
  g_signal_connect (spinbutton_adj_lo, "value_changed",
                    G_CALLBACK (gimp_int_adjustment_update),
                    &thresholds . low_threshold);

  label_hi = gtk_label_new_with_mnemonic ("_High threshold");
  gtk_widget_show (label_hi);
  gtk_table_attach_defaults (GTK_TABLE(table), label_hi, 0, 1, 1, 2);
  gtk_label_set_justify (GTK_LABEL (label_hi), GTK_JUSTIFY_RIGHT);

  spinbutton_hi = gimp_spin_button_new (&spinbutton_adj_hi, thresholds . high_threshold, 1, 255, 1, 1, 1, 5, 0);
  gtk_table_attach_defaults (GTK_TABLE(table), spinbutton_hi, 1, 2, 1, 2);
  gtk_widget_show (spinbutton_hi);

  g_signal_connect_swapped (spinbutton_adj_hi, "value_changed",
                            G_CALLBACK (gimp_preview_invalidate),
                            preview);
  g_signal_connect (spinbutton_adj_hi, "value_changed",
                    G_CALLBACK (gimp_int_adjustment_update),
                    &thresholds . high_threshold);

  frame_label = gtk_label_new ("<b>Thresholds</b>");
//====================================================================================================
  gtk_widget_show (frame_label);
  gtk_frame_set_label_widget (GTK_FRAME (frame), frame_label);
  gtk_label_set_use_markup (GTK_LABEL (frame_label), TRUE);

  g_signal_connect_swapped (preview, "invalidated",
                            G_CALLBACK (canny_detection),
                            drawable);
//----------------------------------------------------------------------------------------------------
  canny_detection (drawable, GIMP_PREVIEW (preview));

  gtk_widget_show (dialog);

  run = (gimp_dialog_run (GIMP_DIALOG (dialog)) == GTK_RESPONSE_OK);

  gtk_widget_destroy (dialog);

  return run;
}
