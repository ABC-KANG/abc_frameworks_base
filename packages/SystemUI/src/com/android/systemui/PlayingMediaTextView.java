/*
 * Copyright (C) 2017 ABC rom
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.systemui;

import android.content.Context;
import android.media.MediaMetadata;
import android.util.AttributeSet;
import android.view.View;
import android.widget.TextView;

import com.android.systemui.R;
import com.android.systemui.statusbar.phone.StatusBar;


public class PlayingMediaTextView extends TextView {
    private static final String TAG = "PlayingMediaTextView";

    private boolean mAvailable;

    private StatusBar mStatusBar;

    public PlayingMediaTextView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
    }

    public void setDark(boolean dark) {
        updateVisibility(dark);
    }

    private void updateVisibility(boolean show) {
        setVisibility(show && mAvailable ? View.VISIBLE : View.GONE);
    }

    public void setPlayingMediaText(MediaMetadata mediaMetaData) {
        if (mediaMetaData != null) {
            CharSequence artist = mediaMetaData.getText(MediaMetadata.METADATA_KEY_ARTIST);
            CharSequence album = mediaMetaData.getText(MediaMetadata.METADATA_KEY_ALBUM);
            CharSequence title = mediaMetaData.getText(MediaMetadata.METADATA_KEY_TITLE);
            if (artist != null && album != null && title != null) {
                mAvailable = true;
                /* considering we are in Ambient mode here, it's probably not worth it to show
                    too many infos, so let's skip album name to keep a smaller text */
                setText(artist.toString() /*+ " - " + album.toString()*/ + " - " + title.toString());
                if (mStatusBar != null) {
                    mStatusBar.triggerAmbientForMedia();
                }
            }
        } else {
            mAvailable = false;
            updateVisibility(false);
        }
    }

    public void setStatusBar(StatusBar bar) {
        mStatusBar = bar;
    }
}
