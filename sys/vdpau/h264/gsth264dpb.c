/* GStreamer
 *
 * Copyright (C) 2009 Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>.
   *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
   *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gsth264dpb.h"

/* Properties */
enum
{
  PROP_0,
  PROP_NUM_REF_FRAMES
};

GST_DEBUG_CATEGORY_STATIC (h264dpb_debug);
#define GST_CAT_DEFAULT h264dpb_debug

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (h264dpb_debug, "h264dpb", 0, \
  "H264 DPB");

G_DEFINE_TYPE_WITH_CODE (GstH264DPB, gst_h264_dpb, G_TYPE_OBJECT, DEBUG_INIT);

void
gst_h264_dpb_fill_reference_frames (GstH264DPB * dpb,
    VdpReferenceFrameH264 reference_frames[16])
{
  GstVdpH264Frame **frames;
  guint i;

  frames = dpb->frames;
  for (i = 0; i < dpb->n_frames; i++) {
    GstVdpH264Frame *frame = frames[i];

    reference_frames[i].surface =
        GST_VDP_VIDEO_BUFFER (GST_VIDEO_FRAME_CAST (frame)->
        src_buffer)->surface;

    reference_frames[i].is_long_term = frame->is_long_term;
    reference_frames[i].top_is_reference = frame->is_reference;
    reference_frames[i].bottom_is_reference = frame->is_reference;
    reference_frames[i].field_order_cnt[0] = frame->poc;
    reference_frames[i].field_order_cnt[1] = frame->poc;
    reference_frames[i].frame_idx = frame->frame_num;
  }

  for (i = dpb->n_frames; i < 16; i++) {
    reference_frames[i].surface = VDP_INVALID_HANDLE;
    reference_frames[i].top_is_reference = VDP_FALSE;
    reference_frames[i].bottom_is_reference = VDP_FALSE;
  }
}

static void
gst_h264_dpb_remove (GstH264DPB * dpb, guint idx)
{
  GstVdpH264Frame **frames;
  guint i;

  frames = dpb->frames;
  gst_video_frame_unref (GST_VIDEO_FRAME_CAST (frames[idx]));
  dpb->n_frames--;

  for (i = idx; i < dpb->n_frames; i++)
    frames[i] = frames[i + 1];
}

static void
gst_h264_dpb_output (GstH264DPB * dpb, guint idx)
{
  GstVdpH264Frame *frame = dpb->frames[idx];

  gst_video_frame_ref (GST_VIDEO_FRAME_CAST (frame));
  dpb->output (dpb, frame);
  frame->output_needed = FALSE;

  if (!frame->is_reference)
    gst_h264_dpb_remove (dpb, idx);
}

static gboolean
gst_h264_dpb_bump (GstH264DPB * dpb, guint poc)
{
  GstVdpH264Frame **frames;
  guint i;
  gint bump_idx;

  frames = dpb->frames;
  bump_idx = -1;
  for (i = 0; i < dpb->n_frames; i++) {
    if (frames[i]->output_needed) {
      bump_idx = i;
      break;
    }
  }

  if (bump_idx != -1) {
    for (i = bump_idx + 1; i < dpb->n_frames; i++) {
      if (frames[i]->output_needed && (frames[i]->poc < frames[bump_idx]->poc)) {
        bump_idx = i;
      }
    }

    if (frames[bump_idx]->poc < poc) {
      gst_h264_dpb_output (dpb, bump_idx);
      return TRUE;
    }
  }

  return FALSE;
}

gboolean
gst_h264_dpb_add (GstH264DPB * dpb, GstVdpH264Frame * h264_frame)
{
  GstVdpH264Frame **frames;

  GST_DEBUG ("add frame with poc: %d", h264_frame->poc);

  frames = dpb->frames;

  if (h264_frame->is_reference) {

    while (dpb->n_frames == dpb->max_frames) {
      if (!gst_h264_dpb_bump (dpb, G_MAXUINT)) {
        GST_ERROR_OBJECT (dpb, "Couldn't make room in DPB");
        return FALSE;
      }
    }
    dpb->frames[dpb->n_frames++] = h264_frame;
  }

  else {
    if (dpb->n_frames == dpb->max_frames) {
      while (gst_h264_dpb_bump (dpb, h264_frame->poc));

      dpb->output (dpb, h264_frame);
    }

    else
      dpb->frames[dpb->n_frames++] = h264_frame;
  }

  return TRUE;
}

void
gst_h264_dpb_flush (GstH264DPB * dpb, gboolean output)
{
  GstVideoFrame **frames;
  guint i;

  GST_DEBUG ("flush");

  if (output)
    while (gst_h264_dpb_bump (dpb, G_MAXUINT));

  frames = (GstVideoFrame **) dpb->frames;
  for (i = 0; i < dpb->n_frames; i++)
    gst_video_frame_unref (frames[i]);

  dpb->n_frames = 0;

}

void
gst_h264_dpb_mark_sliding (GstH264DPB * dpb)
{
  GstVdpH264Frame **frames;
  guint i;
  gint mark_idx;

  if (dpb->n_frames != dpb->max_frames)
    return;

  frames = dpb->frames;
  for (i = 0; i < dpb->n_frames; i++) {
    if (frames[i]->is_reference && !frames[i]->is_long_term) {
      mark_idx = i;
      break;
    }
  }

  if (mark_idx != -1) {
    for (i = mark_idx; i < dpb->n_frames; i++) {
      if (frames[i]->is_reference && !frames[i]->is_long_term &&
          frames[i]->frame_num < frames[mark_idx]->frame_num)
        mark_idx = i;
    }

    frames[mark_idx]->is_reference = FALSE;
    if (!frames[mark_idx]->output_needed)
      gst_h264_dpb_remove (dpb, mark_idx);
  }
}

/* GObject vmethod implementations */
static void
gst_h264_dpb_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstH264DPB *dpb = GST_H264_DPB (object);

  switch (property_id) {
    case PROP_NUM_REF_FRAMES:
      g_value_set_uint (value, dpb->max_frames);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_h264_dpb_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH264DPB *dpb = GST_H264_DPB (object);

  switch (property_id) {
    case PROP_NUM_REF_FRAMES:
    {
      guint i;

      dpb->max_frames = g_value_get_uint (value);
      for (i = dpb->n_frames; i > dpb->max_frames; i--)
        gst_h264_dpb_bump (dpb, G_MAXUINT);

      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_h264_dpb_finalize (GObject * object)
{
  /* TODO: Add deinitalization code here */

  G_OBJECT_CLASS (gst_h264_dpb_parent_class)->finalize (object);
}

static void
gst_h264_dpb_init (GstH264DPB * dpb)
{
  dpb->n_frames = 0;
  dpb->max_frames = MAX_DPB_SIZE;
}

static void
gst_h264_dpb_class_init (GstH264DPBClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gst_h264_dpb_finalize;
  object_class->set_property = gst_h264_dpb_set_property;
  object_class->get_property = gst_h264_dpb_get_property;

  g_object_class_install_property (object_class, PROP_NUM_REF_FRAMES,
      g_param_spec_uint ("num-ref-frames", "Num Ref Frames",
          "How many reference frames the DPB should hold ",
          0, 16, 16, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

}
