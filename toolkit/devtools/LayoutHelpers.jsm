/* -*- Mode: Javascript; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const Cu = Components.utils;
const Ci = Components.interfaces;
const Cr = Components.results;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "Services",
  "resource://gre/modules/Services.jsm");

this.EXPORTED_SYMBOLS = ["LayoutHelpers"];

this.LayoutHelpers = LayoutHelpers = function(aTopLevelWindow) {
  this._topDocShell = aTopLevelWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                                     .getInterface(Ci.nsIWebNavigation)
                                     .QueryInterface(Ci.nsIDocShell);
};

LayoutHelpers.prototype = {

  /**
   * Get box quads adjusted for iframes and zoom level.
   *
   * @param {DOMNode} node
   *        The node for which we are to get the box model region quads
   * @param  {String} region
   *         The box model region to return:
   *         "content", "padding", "border" or "margin"
   */
  getAdjustedQuads: function(node, region) {
    if (!node) {
      return;
    }

    let [quads] = node.getBoxQuads({
      box: region
    });

    if (!quads) {
      return;
    }

    let [xOffset, yOffset] = this._getNodeOffsets(node);
    let scale = this.calculateScale(node);

    return {
      p1: {
        w: quads.p1.w * scale,
        x: quads.p1.x * scale + xOffset,
        y: quads.p1.y * scale + yOffset,
        z: quads.p1.z * scale
      },
      p2: {
        w: quads.p2.w * scale,
        x: quads.p2.x * scale + xOffset,
        y: quads.p2.y * scale + yOffset,
        z: quads.p2.z * scale
      },
      p3: {
        w: quads.p3.w * scale,
        x: quads.p3.x * scale + xOffset,
        y: quads.p3.y * scale + yOffset,
        z: quads.p3.z * scale
      },
      p4: {
        w: quads.p4.w * scale,
        x: quads.p4.x * scale + xOffset,
        y: quads.p4.y * scale + yOffset,
        z: quads.p4.z * scale
      },
      bounds: {
        bottom: quads.bounds.bottom * scale + yOffset,
        height: quads.bounds.height * scale,
        left: quads.bounds.left * scale + xOffset,
        right: quads.bounds.right * scale + xOffset,
        top: quads.bounds.top * scale + yOffset,
        width: quads.bounds.width * scale,
        x: quads.bounds.x * scale + xOffset,
        y: quads.bounds.y * scale + yOffset
      }
    };
  },

  calculateScale: function(node) {
    let win = node.ownerDocument.defaultView;
    let winUtils = win.QueryInterface(Ci.nsIInterfaceRequestor)
                      .getInterface(Ci.nsIDOMWindowUtils);
    return winUtils.fullZoom;
  },

  /**
   * Compute the absolute position and the dimensions of a node, relativalely
   * to the root window.
   *
   * @param nsIDOMNode aNode
   *        a DOM element to get the bounds for
   * @param nsIWindow aContentWindow
   *        the content window holding the node
   */
  getRect: function LH_getRect(aNode, aContentWindow) {
    let frameWin = aNode.ownerDocument.defaultView;
    let clientRect = aNode.getBoundingClientRect();

    // Go up in the tree of frames to determine the correct rectangle.
    // clientRect is read-only, we need to be able to change properties.
    let rect = {top: clientRect.top + aContentWindow.pageYOffset,
            left: clientRect.left + aContentWindow.pageXOffset,
            width: clientRect.width,
            height: clientRect.height};

    // We iterate through all the parent windows.
    while (true) {

      // Are we in the top-level window?
      if (this.isTopLevelWindow(frameWin)) {
        break;
      }

      let frameElement = this.getFrameElement(frameWin);
      if (!frameElement) {
        break;
      }

      // We are in an iframe.
      // We take into account the parent iframe position and its
      // offset (borders and padding).
      let frameRect = frameElement.getBoundingClientRect();

      let [offsetTop, offsetLeft] =
        this.getIframeContentOffset(frameElement);

      rect.top += frameRect.top + offsetTop;
      rect.left += frameRect.left + offsetLeft;

      frameWin = this.getParentWindow(frameWin);
    }

    return rect;
  },

  /**
   * Returns iframe content offset (iframe border + padding).
   * Note: this function shouldn't need to exist, had the platform provided a
   * suitable API for determining the offset between the iframe's content and
   * its bounding client rect. Bug 626359 should provide us with such an API.
   *
   * @param aIframe
   *        The iframe.
   * @returns array [offsetTop, offsetLeft]
   *          offsetTop is the distance from the top of the iframe and the
   *            top of the content document.
   *          offsetLeft is the distance from the left of the iframe and the
   *            left of the content document.
   */
  getIframeContentOffset: function LH_getIframeContentOffset(aIframe) {
    let style = aIframe.contentWindow.getComputedStyle(aIframe, null);

    // In some cases, the computed style is null
    if (!style) {
      return [0, 0];
    }

    let paddingTop = parseInt(style.getPropertyValue("padding-top"));
    let paddingLeft = parseInt(style.getPropertyValue("padding-left"));

    let borderTop = parseInt(style.getPropertyValue("border-top-width"));
    let borderLeft = parseInt(style.getPropertyValue("border-left-width"));

    return [borderTop + paddingTop, borderLeft + paddingLeft];
  },

  /**
   * Find an element from the given coordinates. This method descends through
   * frames to find the element the user clicked inside frames.
   *
   * @param DOMDocument aDocument the document to look into.
   * @param integer aX
   * @param integer aY
   * @returns Node|null the element node found at the given coordinates.
   */
  getElementFromPoint: function LH_elementFromPoint(aDocument, aX, aY) {
    let node = aDocument.elementFromPoint(aX, aY);
    if (node && node.contentDocument) {
      if (node instanceof Ci.nsIDOMHTMLIFrameElement) {
        let rect = node.getBoundingClientRect();

        // Gap between the iframe and its content window.
        let [offsetTop, offsetLeft] = this.getIframeContentOffset(node);

        aX -= rect.left + offsetLeft;
        aY -= rect.top + offsetTop;

        if (aX < 0 || aY < 0) {
          // Didn't reach the content document, still over the iframe.
          return node;
        }
      }
      if (node instanceof Ci.nsIDOMHTMLIFrameElement ||
          node instanceof Ci.nsIDOMHTMLFrameElement) {
        let subnode = this.getElementFromPoint(node.contentDocument, aX, aY);
        if (subnode) {
          node = subnode;
        }
      }
    }
    return node;
  },

  /**
   * Scroll the document so that the element "elem" appears in the viewport.
   *
   * @param Element elem the element that needs to appear in the viewport.
   * @param bool centered true if you want it centered, false if you want it to
   * appear on the top of the viewport. It is true by default, and that is
   * usually what you want.
   */
  scrollIntoViewIfNeeded: function(elem, centered) {
    // We want to default to centering the element in the page,
    // so as to keep the context of the element.
    centered = centered === undefined? true: !!centered;

    let win = elem.ownerDocument.defaultView;
    let clientRect = elem.getBoundingClientRect();

    // The following are always from the {top, bottom, left, right}
    // of the viewport, to the {top, ???} of the box.
    // Think of them as geometrical vectors, it helps.
    // The origin is at the top left.

    let topToBottom = clientRect.bottom;
    let bottomToTop = clientRect.top - win.innerHeight;
    let leftToRight = clientRect.right;
    let rightToLeft = clientRect.left - win.innerWidth;
    let xAllowed = true;  // We allow one translation on the x axis,
    let yAllowed = true;  // and one on the y axis.

    // Whatever `centered` is, the behavior is the same if the box is
    // (even partially) visible.

    if ((topToBottom > 0 || !centered) && topToBottom <= elem.offsetHeight) {
      win.scrollBy(0, topToBottom - elem.offsetHeight);
      yAllowed = false;
    } else
    if ((bottomToTop < 0 || !centered) && bottomToTop >= -elem.offsetHeight) {
      win.scrollBy(0, bottomToTop + elem.offsetHeight);
      yAllowed = false;
    }

    if ((leftToRight > 0 || !centered) && leftToRight <= elem.offsetWidth) {
      if (xAllowed) {
        win.scrollBy(leftToRight - elem.offsetWidth, 0);
        xAllowed = false;
      }
    } else
    if ((rightToLeft < 0 || !centered) && rightToLeft >= -elem.offsetWidth) {
      if (xAllowed) {
        win.scrollBy(rightToLeft + elem.offsetWidth, 0);
        xAllowed = false;
      }
    }

    // If we want it centered, and the box is completely hidden,
    // then we center it explicitly.

    if (centered) {

      if (yAllowed && (topToBottom <= 0 || bottomToTop >= 0)) {
        win.scroll(win.scrollX,
                   win.scrollY + clientRect.top
                   - (win.innerHeight - elem.offsetHeight) / 2);
      }

      if (xAllowed && (leftToRight <= 0 || rightToLeft <= 0)) {
        win.scroll(win.scrollX + clientRect.left
                   - (win.innerWidth - elem.offsetWidth) / 2,
                   win.scrollY);
      }
    }

    if (!this.isTopLevelWindow(win)) {
      // We are inside an iframe.
      let frameElement = this.getFrameElement(win);
      this.scrollIntoViewIfNeeded(frameElement, centered);
    }
  },

  /**
   * Check if a node and its document are still alive
   * and attached to the window.
   *
   * @param aNode
   */
  isNodeConnected: function LH_isNodeConnected(aNode)
  {
    try {
      let connected = (aNode.ownerDocument && aNode.ownerDocument.defaultView &&
                      !(aNode.compareDocumentPosition(aNode.ownerDocument.documentElement) &
                      aNode.DOCUMENT_POSITION_DISCONNECTED));
      return connected;
    } catch (e) {
      // "can't access dead object" error
      return false;
    }
  },

  /**
   * like win.parent === win, but goes through mozbrowsers and mozapps iframes.
   */
  isTopLevelWindow: function LH_isTopLevelWindow(win) {
    let docShell = win.QueryInterface(Ci.nsIInterfaceRequestor)
                   .getInterface(Ci.nsIWebNavigation)
                   .QueryInterface(Ci.nsIDocShell);

    return docShell === this._topDocShell;
  },

  /**
   * Check a window is part of the top level window.
   */
  isIncludedInTopLevelWindow: function LH_isIncludedInTopLevelWindow(win) {
    if (this.isTopLevelWindow(win)) {
      return true;
    }

    let parent = this.getParentWindow(win);
    if (!parent || parent === win) {
      return false;
    }

    return this.isIncludedInTopLevelWindow(parent);
  },

  /**
   * like win.parent, but goes through mozbrowsers and mozapps iframes.
   */
  getParentWindow: function LH_getParentWindow(win) {
    if (this.isTopLevelWindow(win)) {
      return null;
    }

    let docShell = win.QueryInterface(Ci.nsIInterfaceRequestor)
                   .getInterface(Ci.nsIWebNavigation)
                   .QueryInterface(Ci.nsIDocShell);

    if (docShell.isBrowserOrApp) {
      let parentDocShell = docShell.getSameTypeParentIgnoreBrowserAndAppBoundaries();
      return parentDocShell ? parentDocShell.contentViewer.DOMDocument.defaultView : null;
    } else {
      return win.parent;
    }
  },

  /**
   * like win.frameElement, but goes through mozbrowsers and mozapps iframes.
   *
   * @param DOMWindow win The window to get the frame for
   * @return DOMElement The element in which the window is embedded.
   */
  getFrameElement: function LH_getFrameElement(win) {
    if (this.isTopLevelWindow(win)) {
      return null;
    }

    let winUtils = win.
      QueryInterface(Components.interfaces.nsIInterfaceRequestor).
      getInterface(Components.interfaces.nsIDOMWindowUtils);

    return winUtils.containerElement;
  },

  /**
   * Get the x and y offsets for a node taking iframes into account.
   *
   * @param {DOMNode} node
   *        The node for which we are to get the offset
   */
  _getNodeOffsets: function(node) {
    let xOffset = 0;
    let yOffset = 0;
    let frameWin = node.ownerDocument.defaultView;
    let scale = this.calculateScale(node);

    while (true) {
      // Are we in the top-level window?
      if (this.isTopLevelWindow(frameWin)) {
        break;
      }

      let frameElement = this.getFrameElement(frameWin);
      if (!frameElement) {
        break;
      }

      // We are in an iframe.
      // We take into account the parent iframe position and its
      // offset (borders and padding).
      let frameRect = frameElement.getBoundingClientRect();

      let [offsetTop, offsetLeft] =
        this.getIframeContentOffset(frameElement);

      xOffset += frameRect.left + offsetLeft;
      yOffset += frameRect.top + offsetTop;

      frameWin = this.getParentWindow(frameWin);
    }

    return [xOffset * scale, yOffset * scale];
  },



  /********************************************************************
   * GetBoxQuads POLYFILL START TODO: Remove this when bug 917755 is fixed.
   ********************************************************************/
  _getBoxQuadsFromRect: function(rect, node) {
    let scale = this.calculateScale(node);
    let [xOffset, yOffset] = this._getNodeOffsets(node);

    let out = {
      p1: {
        x: rect.left * scale + xOffset,
        y: rect.top * scale + yOffset
      },
      p2: {
        x: (rect.left + rect.width) * scale + xOffset,
        y: rect.top * scale + yOffset
      },
      p3: {
        x: (rect.left + rect.width) * scale + xOffset,
        y: (rect.top + rect.height) * scale + yOffset
      },
      p4: {
        x: rect.left * scale + xOffset,
        y: (rect.top + rect.height) * scale + yOffset
      }
    };

    out.bounds = {
      bottom: out.p4.y,
      height: out.p4.y - out.p1.y,
      left: out.p1.x,
      right: out.p2.x,
      top: out.p1.y,
      width: out.p2.x - out.p1.x,
      x: out.p1.x,
      y: out.p1.y
    };

    return out;
  },

  _parseNb: function(distance) {
    let nb = parseFloat(distance, 10);
    return isNaN(nb) ? 0 : nb;
  },

  getAdjustedQuadsPolyfill: function(node, region) {
    // Get the border-box rect
    // Note that this is relative to the node's viewport, so before we can use
    // it, will need to go back up the frames like getRect
    let borderRect = node.getBoundingClientRect();

    // If the boxType is border, no need to go any further, we're done
    if (region === "border") {
      return this._getBoxQuadsFromRect(borderRect, node);
    }

    // Else, need to get margin/padding/border distances
    let style = node.ownerDocument.defaultView.getComputedStyle(node);
    let camel = s => s.substring(0, 1).toUpperCase() + s.substring(1);
    let distances = {border:{}, padding:{}, margin: {}};

    for (let side of ["top", "right", "bottom", "left"]) {
      distances.border[side] = this._parseNb(style["border" + camel(side) + "Width"]);
      distances.padding[side] = this._parseNb(style["padding" + camel(side)]);
      distances.margin[side] = this._parseNb(style["margin" + camel(side)]);
    }

    // From the border-box rect, calculate the content-box, padding-box and
    // margin-box rects
    function offsetRect(rect, offsetType, dir=1) {
      return {
        top: rect.top + (dir * distances[offsetType].top),
        left: rect.left + (dir * distances[offsetType].left),
        width: rect.width - (dir * (distances[offsetType].left + distances[offsetType].right)),
        height: rect.height - (dir * (distances[offsetType].top + distances[offsetType].bottom))
      };
    }

    if (region === "margin") {
      return this._getBoxQuadsFromRect(offsetRect(borderRect, "margin", -1), node);
    } else if (region === "padding") {
      return this._getBoxQuadsFromRect(offsetRect(borderRect, "border"), node);
    } else if (region === "content") {
      let paddingRect = offsetRect(borderRect, "border");
      return this._getBoxQuadsFromRect(offsetRect(paddingRect, "padding"), node);
    }
  },

  /********************************************************************
   * GetBoxQuads POLYFILL END
   ********************************************************************/
};
