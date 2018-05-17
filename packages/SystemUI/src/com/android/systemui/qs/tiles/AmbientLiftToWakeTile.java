/*
 * Copyright (C) 2017 The ABC rom
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

package com.android.systemui.qs.tiles;

import android.content.Intent;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.UserHandle;
import android.provider.Settings;
import android.service.quicksettings.Tile;

import com.android.internal.hardware.AmbientDisplayConfiguration;
import com.android.internal.logging.nano.MetricsProto.MetricsEvent;
import com.android.internal.util.abc.AbcUtils;
import com.android.systemui.Prefs;
import com.android.systemui.qs.QSHost;
import com.android.systemui.plugins.qs.QSTile.BooleanState;
import com.android.systemui.qs.tileimpl.QSTileImpl;
import com.android.systemui.R;

/** Quick settings tile: Ambient and LiftToWake mode **/
public class AmbientLiftToWakeTile extends QSTileImpl<BooleanState> {

    private final Icon mIcon = ResourceIcon.get(R.drawable.ic_qs_ambient_on);

    private final String DOZE_ENABLED = Settings.Secure.DOZE_ENABLED;
    private final String DOZE_PULSE_ON_PICK_UP = Settings.Secure.DOZE_PULSE_ON_PICK_UP;
    private final String CUSTOM_AMBIENT_HANDWAVE_GESTURE = Settings.System.CUSTOM_AMBIENT_HANDWAVE_GESTURE;
    private final String CUSTOM_AMBIENT_POCKETMODE_GESTURE = Settings.System.CUSTOM_AMBIENT_POCKETMODE_GESTURE;

    private AmbientDisplayConfiguration mAmbientConfig;
    private boolean isAmbientAvailable;
    private boolean isPickupAvailable;
    private boolean areCustomAmbientGesturesAvailable;
    private boolean isSomethingEnabled() {
        boolean enabled = false;
        if (isAmbientAvailable) {
            enabled = Settings.Secure.getIntForUser(mContext.getContentResolver(),
                    DOZE_ENABLED, 1, UserHandle.USER_CURRENT) == 1;
        }
        if (isPickupAvailable && !enabled) {
            enabled = Settings.Secure.getIntForUser(mContext.getContentResolver(),
                    DOZE_PULSE_ON_PICK_UP, 1, UserHandle.USER_CURRENT) == 1;
        }
        if (areCustomAmbientGesturesAvailable && !enabled) {
            enabled = Settings.System.getIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_HANDWAVE_GESTURE, 0, UserHandle.USER_CURRENT) == 1 ||
                    Settings.System.getIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_POCKETMODE_GESTURE, 0, UserHandle.USER_CURRENT) == 1;
        }

        return enabled ? true : false;
    }

    public AmbientLiftToWakeTile(QSHost host) {
        super(host);
        mAmbientConfig = new AmbientDisplayConfiguration(mContext);
        // this will be true also for custom ambient components
        isAmbientAvailable =  mAmbientConfig.pulseOnNotificationAvailable() ? true : false;
        isPickupAvailable = mAmbientConfig.pulseOnPickupAvailable() ? true : false;
        areCustomAmbientGesturesAvailable = AbcUtils.hasAltAmbientDisplay(mContext);
    }

    @Override
    public BooleanState newTileState() {
        return new BooleanState();
    }

    @Override
    protected void handleClick() {
        if (isSomethingEnabled()) {
            getUserDozeValues();
            setDisabled();
        } else {
            setUserValues();
        }
    }

    @Override
    public boolean isAvailable() {
        //do not show the tile if no doze features available
        return isAmbientAvailable;
    }

    private void setDisabled() {
        if (isAmbientAvailable) {
            Settings.Secure.putIntForUser(mContext.getContentResolver(),
                    DOZE_ENABLED, 0, UserHandle.USER_CURRENT);
        }
        if (isPickupAvailable) {
            Settings.Secure.putIntForUser(mContext.getContentResolver(),
                    DOZE_PULSE_ON_PICK_UP, 0, UserHandle.USER_CURRENT);
        }
        if (areCustomAmbientGesturesAvailable) {
            Settings.System.putIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_HANDWAVE_GESTURE, 0, UserHandle.USER_CURRENT);
            Settings.System.putIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_POCKETMODE_GESTURE, 0, UserHandle.USER_CURRENT);
        }
    }

    private void setUserValues() {
        if (isAmbientAvailable) {
            Settings.Secure.putIntForUser(mContext.getContentResolver(),
                    DOZE_ENABLED, Prefs.getInt(mContext, Prefs.Key.QS_AMBIENT_DOZE, 1),
                    UserHandle.USER_CURRENT);
        }
        if (isPickupAvailable) {
            Settings.Secure.putIntForUser(mContext.getContentResolver(),
                    DOZE_PULSE_ON_PICK_UP, Prefs.getInt(mContext, Prefs.Key.QS_AMBIENT_PICKUP, 1),
                    UserHandle.USER_CURRENT);
        }
        if (areCustomAmbientGesturesAvailable) {
            Settings.System.putIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_HANDWAVE_GESTURE, Prefs.getInt(mContext, Prefs.Key.QS_AMBIENT_HANDWAVE, 1),
                    UserHandle.USER_CURRENT);
            Settings.System.putIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_POCKETMODE_GESTURE, Prefs.getInt(mContext, Prefs.Key.QS_AMBIENT_POCKETMODE, 1),
                    UserHandle.USER_CURRENT);
        }
    }

    private void getUserDozeValues() {
        int value = 0;
        if (isAmbientAvailable) {
            value = Settings.Secure.getIntForUser(mContext.getContentResolver(),
                    DOZE_ENABLED, 1, UserHandle.USER_CURRENT);
            Prefs.putInt(mContext, Prefs.Key.QS_AMBIENT_DOZE, value);
        }

        if (isPickupAvailable) {
            value =  Settings.Secure.getIntForUser(mContext.getContentResolver(),
                    DOZE_PULSE_ON_PICK_UP, 1, UserHandle.USER_CURRENT);
            Prefs.putInt(mContext, Prefs.Key.QS_AMBIENT_PICKUP, value);
        }

        if (areCustomAmbientGesturesAvailable) {
            value = Settings.System.getIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_HANDWAVE_GESTURE, 0, UserHandle.USER_CURRENT);
            Prefs.putInt(mContext, Prefs.Key.QS_AMBIENT_HANDWAVE, value);

            value = Settings.System.getIntForUser(mContext.getContentResolver(),
                    CUSTOM_AMBIENT_POCKETMODE_GESTURE, 0, UserHandle.USER_CURRENT);
            Prefs.putInt(mContext, Prefs.Key.QS_AMBIENT_POCKETMODE, value);
        }
    }


    @Override
    public Intent getLongClickIntent() {
        return new Intent(Settings.ACTION_DISPLAY_SETTINGS);
    }

    @Override
    public CharSequence getTileLabel() {
        return mContext.getString(R.string.quick_settings_doze_notifications_label);
    }

    @Override
    protected void handleUpdateState(BooleanState state, Object arg) {
        if (mAmbientConfig == null) {
            return;
        }
        if (state.slash == null) {
            state.slash = new SlashState();
        }
        state.icon = mIcon;
        if (isSomethingEnabled()) {
            getUserDozeValues();
            state.label = mContext.getString(R.string.quick_settings_doze_notifications_label);
            state.contentDescription =  mContext.getString(
                    R.string.quick_settings_doze_notifications_label);
            state.slash.isSlashed = false;
            state.state = Tile.STATE_ACTIVE;
        } else {
            state.label = mContext.getString(R.string.quick_settings_doze_notifications_label);
            state.contentDescription =  mContext.getString(
                    R.string.quick_settings_doze_notifications_label);
            state.slash.isSlashed = true;
            state.state = Tile.STATE_INACTIVE;
        }
    }

    @Override
    public int getMetricsCategory() {
        return MetricsEvent.ABC;
    }

    private ContentObserver mObserver = new ContentObserver(mHandler) {
        @Override
        public void onChange(boolean selfChange, Uri uri) {
            refreshState();
        }
    };

    @Override
    public void destroy() {
        mContext.getContentResolver().unregisterContentObserver(mObserver);
    }

    @Override
    public void handleSetListening(boolean listening) {
        if (mAmbientConfig == null) {
            return;
        }
        if (listening) {
            if (isAmbientAvailable) {
                mContext.getContentResolver().registerContentObserver(
                        Settings.Secure.getUriFor(DOZE_ENABLED),
                        false, mObserver, UserHandle.USER_ALL);
            }
            if (isPickupAvailable) {
                mContext.getContentResolver().registerContentObserver(
                        Settings.Secure.getUriFor(DOZE_PULSE_ON_PICK_UP),
                        false, mObserver, UserHandle.USER_ALL);
            }
            if (areCustomAmbientGesturesAvailable) {
                mContext.getContentResolver().registerContentObserver(
                        Settings.System.getUriFor(CUSTOM_AMBIENT_HANDWAVE_GESTURE),
                        false, mObserver, UserHandle.USER_ALL);
                mContext.getContentResolver().registerContentObserver(
                        Settings.System.getUriFor(CUSTOM_AMBIENT_POCKETMODE_GESTURE),
                        false, mObserver, UserHandle.USER_ALL);
            }
        } else {
            mContext.getContentResolver().unregisterContentObserver(mObserver);
        }
    }
}
