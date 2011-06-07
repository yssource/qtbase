/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qxcbintegration.h"
#include "qxcbconnection.h"
#include "qxcbscreen.h"
#include "qxcbwindow.h"
#include "qxcbwindowsurface.h"
#include "qxcbnativeinterface.h"
#include "qxcbclipboard.h"
#include "qxcbdrag.h"
#include "qxcbimage.h"

#include <qgenericunixprintersupport.h>

#include <xcb/xcb.h>

#include <private/qpixmap_raster_p.h>

#include "qgenericunixfontdatabase.h"

#include <stdio.h>

#ifdef XCB_USE_EGL
#include <EGL/egl.h>
#endif

QXcbIntegration::QXcbIntegration()
    : m_connection(new QXcbConnection), m_printerSupport(new QGenericUnixPrinterSupport)
{
    foreach (QXcbScreen *screen, m_connection->screens())
        m_screens << screen;

    m_fontDatabase = new QGenericUnixFontDatabase();
    m_nativeInterface = new QXcbNativeInterface;
}

QXcbIntegration::~QXcbIntegration()
{
    delete m_connection;
}

bool QXcbIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps: return true;
    case OpenGL: return hasOpenGL();
    default: return QPlatformIntegration::hasCapability(cap);
    }
}

QPixmapData *QXcbIntegration::createPixmapData(QPixmapData::PixelType type) const
{
    return new QRasterPixmapData(type);
}

QPlatformWindow *QXcbIntegration::createPlatformWindow(QWindow *window) const
{
    return new QXcbWindow(window);
}

QWindowSurface *QXcbIntegration::createWindowSurface(QWindow *window, WId winId) const
{
    Q_UNUSED(winId);
    return new QXcbWindowSurface(window);
}

QList<QPlatformScreen *> QXcbIntegration::screens() const
{
    return m_screens;
}

void QXcbIntegration::moveToScreen(QWindow *window, int screen)
{
    Q_UNUSED(window);
    Q_UNUSED(screen);
}

bool QXcbIntegration::isVirtualDesktop()
{
    return false;
}

QPlatformFontDatabase *QXcbIntegration::fontDatabase() const
{
    return m_fontDatabase;
}

QPixmap QXcbIntegration::grabWindow(WId window, int x, int y, int width, int height) const
{
    if (width == 0 || height == 0)
        return QPixmap();

    xcb_connection_t *connection = m_connection->xcb_connection();

    xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(connection, window);

    xcb_generic_error_t *error;
    xcb_get_geometry_reply_t *reply =
        xcb_get_geometry_reply(connection, geometry_cookie, &error);

    if (!reply) {
        if (error) {
            m_connection->handleXcbError(error);
            free(error);
        }
        return QPixmap();
    }

    if (width < 0)
        width = reply->width - x;
    if (height < 0)
        height = reply->height - y;

    // TODO: handle multiple screens
    QXcbScreen *screen = m_connection->screens().at(0);
    xcb_window_t root = screen->root();
    geometry_cookie = xcb_get_geometry(connection, root);
    xcb_get_geometry_reply_t *root_reply =
        xcb_get_geometry_reply(connection, geometry_cookie, &error);

    if (!root_reply) {
        if (error) {
            m_connection->handleXcbError(error);
            free(error);
        }
        free(reply);
        return QPixmap();
    }

    if (reply->depth == root_reply->depth) {
        // if the depth of the specified window and the root window are the
        // same, grab pixels from the root window (so that we get the any
        // overlapping windows and window manager frames)

        // map x and y to the root window
        xcb_translate_coordinates_cookie_t translate_cookie =
            xcb_translate_coordinates(connection, window, root, x, y);

        xcb_translate_coordinates_reply_t *translate_reply =
            xcb_translate_coordinates_reply(connection, translate_cookie, &error);

        if (!translate_reply) {
            if (error) {
                m_connection->handleXcbError(error);
                free(error);
            }
            free(reply);
            free(root_reply);
            return QPixmap();
        }

        x = translate_reply->dst_x;
        y = translate_reply->dst_y;

        window = root;

        free(translate_reply);
        free(reply);
        reply = root_reply;
    } else {
        free(root_reply);
        root_reply = 0;
    }

    xcb_get_window_attributes_reply_t *attributes_reply =
        xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, window), &error);

    if (!attributes_reply) {
        if (error) {
            m_connection->handleXcbError(error);
            free(error);
        }
        free(reply);
        return QPixmap();
    }

    const xcb_visualtype_t *visual = screen->visualForId(attributes_reply->visual);
    free(attributes_reply);

    xcb_pixmap_t pixmap = xcb_generate_id(connection);
    error = xcb_request_check(connection, xcb_create_pixmap_checked(connection, reply->depth, pixmap, window, width, height));
    if (error) {
        m_connection->handleXcbError(error);
        free(error);
    }

    uint32_t gc_value_mask = XCB_GC_SUBWINDOW_MODE;
    uint32_t gc_value_list[] = { XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS };

    xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, pixmap, gc_value_mask, gc_value_list);

    error = xcb_request_check(connection, xcb_copy_area_checked(connection, window, pixmap, gc, x, y, 0, 0, width, height));
    if (error) {
        m_connection->handleXcbError(error);
        free(error);
    }

    QPixmap result = qt_xcb_pixmapFromXPixmap(m_connection, pixmap, width, height, reply->depth, visual);

    free(reply);
    xcb_free_gc(connection, gc);
    xcb_free_pixmap(connection, pixmap);

    return result;
}

bool QXcbIntegration::hasOpenGL() const
{
#if defined(XCB_USE_GLX)
    return true;
#elif defined(XCB_USE_EGL)
    return m_connection->hasEgl();
#elif defined(XCB_USE_DRI2)
    if (m_connection->hasSupportForDri2()) {
        return true;
    }
#endif
    return false;
}

QPlatformNativeInterface * QXcbIntegration::nativeInterface() const
{
    return m_nativeInterface;
}

QPlatformPrinterSupport *QXcbIntegration::printerSupport() const
{
    return m_printerSupport;
}

QPlatformClipboard *QXcbIntegration::clipboard() const
{
    return m_connection->clipboard();
}

QPlatformDrag *QXcbIntegration::drag() const
{
    return m_connection->drag();
}
