/* -*- js2-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {Cc, Ci, Cu, components} = require("chrome");

Cu.import("resource://gre/modules/XPCOMUtils.jsm");

loader.lazyImporter(this, "Services", "resource://gre/modules/Services.jsm");
loader.lazyImporter(this, "LayoutHelpers", "resource://gre/modules/devtools/LayoutHelpers.jsm");

// TODO: Bug 842672 - toolkit/ imports modules from browser/.
// Note that these are only used in JSTermHelpers, see $0 and pprint().
loader.lazyImporter(this, "gDevTools", "resource:///modules/devtools/gDevTools.jsm");
loader.lazyImporter(this, "devtools", "resource://gre/modules/devtools/Loader.jsm");
loader.lazyImporter(this, "VariablesView", "resource:///modules/devtools/VariablesView.jsm");
loader.lazyImporter(this, "DevToolsUtils", "resource://gre/modules/devtools/DevToolsUtils.jsm");

// Match the function name from the result of toString() or toSource().
//
// Examples:
// (function foobar(a, b) { ...
// function foobar2(a) { ...
// function() { ...
const REGEX_MATCH_FUNCTION_NAME = /^\(?function\s+([^(\s]+)\s*\(/;

// Match the function arguments from the result of toString() or toSource().
const REGEX_MATCH_FUNCTION_ARGS = /^\(?function\s*[^\s(]*\s*\((.+?)\)/;

let WebConsoleUtils = {
  /**
   * Convenience function to unwrap a wrapped object.
   *
   * @param aObject the object to unwrap.
   * @return aObject unwrapped.
   */
  unwrap: function WCU_unwrap(aObject)
  {
    try {
      return XPCNativeWrapper.unwrap(aObject);
    }
    catch (ex) {
      return aObject;
    }
  },

  /**
   * Wrap a string in an nsISupportsString object.
   *
   * @param string aString
   * @return nsISupportsString
   */
  supportsString: function WCU_supportsString(aString)
  {
    let str = Cc["@mozilla.org/supports-string;1"].
              createInstance(Ci.nsISupportsString);
    str.data = aString;
    return str;
  },

  /**
   * Clone an object.
   *
   * @param object aObject
   *        The object you want cloned.
   * @param boolean aRecursive
   *        Tells if you want to dig deeper into the object, to clone
   *        recursively.
   * @param function [aFilter]
   *        Optional, filter function, called for every property. Three
   *        arguments are passed: key, value and object. Return true if the
   *        property should be added to the cloned object. Return false to skip
   *        the property.
   * @return object
   *         The cloned object.
   */
  cloneObject: function WCU_cloneObject(aObject, aRecursive, aFilter)
  {
    if (typeof aObject != "object") {
      return aObject;
    }

    let temp;

    if (Array.isArray(aObject)) {
      temp = [];
      Array.forEach(aObject, function(aValue, aIndex) {
        if (!aFilter || aFilter(aIndex, aValue, aObject)) {
          temp.push(aRecursive ? WCU_cloneObject(aValue) : aValue);
        }
      });
    }
    else {
      temp = {};
      for (let key in aObject) {
        let value = aObject[key];
        if (aObject.hasOwnProperty(key) &&
            (!aFilter || aFilter(key, value, aObject))) {
          temp[key] = aRecursive ? WCU_cloneObject(value) : value;
        }
      }
    }

    return temp;
  },

  /**
   * Copies certain style attributes from one element to another.
   *
   * @param nsIDOMNode aFrom
   *        The target node.
   * @param nsIDOMNode aTo
   *        The destination node.
   */
  copyTextStyles: function WCU_copyTextStyles(aFrom, aTo)
  {
    let win = aFrom.ownerDocument.defaultView;
    let style = win.getComputedStyle(aFrom);
    aTo.style.fontFamily = style.getPropertyCSSValue("font-family").cssText;
    aTo.style.fontSize = style.getPropertyCSSValue("font-size").cssText;
    aTo.style.fontWeight = style.getPropertyCSSValue("font-weight").cssText;
    aTo.style.fontStyle = style.getPropertyCSSValue("font-style").cssText;
  },

  /**
   * Gets the ID of the inner window of this DOM window.
   *
   * @param nsIDOMWindow aWindow
   * @return integer
   *         Inner ID for the given aWindow.
   */
  getInnerWindowId: function WCU_getInnerWindowId(aWindow)
  {
    return aWindow.QueryInterface(Ci.nsIInterfaceRequestor).
             getInterface(Ci.nsIDOMWindowUtils).currentInnerWindowID;
  },

  /**
   * Recursively gather a list of inner window ids given a
   * top level window.
   *
   * @param nsIDOMWindow aWindow
   * @return Array
   *         list of inner window ids.
   */
  getInnerWindowIDsForFrames: function WCU_getInnerWindowIDsForFrames(aWindow)
  {
    let innerWindowID = this.getInnerWindowId(aWindow);
    let ids = [innerWindowID];

    if (aWindow.frames) {
      for (let i = 0; i < aWindow.frames.length; i++) {
        let frame = aWindow.frames[i];
        ids = ids.concat(this.getInnerWindowIDsForFrames(frame));
      }
    }

    return ids;
  },


  /**
   * Gets the ID of the outer window of this DOM window.
   *
   * @param nsIDOMWindow aWindow
   * @return integer
   *         Outer ID for the given aWindow.
   */
  getOuterWindowId: function WCU_getOuterWindowId(aWindow)
  {
    return aWindow.QueryInterface(Ci.nsIInterfaceRequestor).
           getInterface(Ci.nsIDOMWindowUtils).outerWindowID;
  },

  /**
   * Abbreviates the given source URL so that it can be displayed flush-right
   * without being too distracting.
   *
   * @param string aSourceURL
   *        The source URL to shorten.
   * @param object [aOptions]
   *        Options:
   *        - onlyCropQuery: boolean that tells if the URL abbreviation function
   *        should only remove the query parameters and the hash fragment from
   *        the given URL.
   * @return string
   *         The abbreviated form of the source URL.
   */
  abbreviateSourceURL:
  function WCU_abbreviateSourceURL(aSourceURL, aOptions = {})
  {
    if (!aOptions.onlyCropQuery && aSourceURL.substr(0, 5) == "data:") {
      let commaIndex = aSourceURL.indexOf(",");
      if (commaIndex > -1) {
        aSourceURL = "data:" + aSourceURL.substring(commaIndex + 1);
      }
    }

    // Remove any query parameters.
    let hookIndex = aSourceURL.indexOf("?");
    if (hookIndex > -1) {
      aSourceURL = aSourceURL.substring(0, hookIndex);
    }

    // Remove any hash fragments.
    let hashIndex = aSourceURL.indexOf("#");
    if (hashIndex > -1) {
      aSourceURL = aSourceURL.substring(0, hashIndex);
    }

    // Remove a trailing "/".
    if (aSourceURL[aSourceURL.length - 1] == "/") {
      aSourceURL = aSourceURL.replace(/\/+$/, "");
    }

    // Remove all but the last path component.
    if (!aOptions.onlyCropQuery) {
      let slashIndex = aSourceURL.lastIndexOf("/");
      if (slashIndex > -1) {
        aSourceURL = aSourceURL.substring(slashIndex + 1);
      }
    }

    return aSourceURL;
  },

  /**
   * Tells if the given function is native or not.
   *
   * @param function aFunction
   *        The function you want to check if it is native or not.
   * @return boolean
   *         True if the given function is native, false otherwise.
   */
  isNativeFunction: function WCU_isNativeFunction(aFunction)
  {
    return typeof aFunction == "function" && !("prototype" in aFunction);
  },

  /**
   * Tells if the given property of the provided object is a non-native getter or
   * not.
   *
   * @param object aObject
   *        The object that contains the property.
   * @param string aProp
   *        The property you want to check if it is a getter or not.
   * @return boolean
   *         True if the given property is a getter, false otherwise.
   */
  isNonNativeGetter: function WCU_isNonNativeGetter(aObject, aProp)
  {
    if (typeof aObject != "object") {
      return false;
    }
    let desc = this.getPropertyDescriptor(aObject, aProp);
    return desc && desc.get && !this.isNativeFunction(desc.get);
  },

  /**
   * Get the property descriptor for the given object.
   *
   * @param object aObject
   *        The object that contains the property.
   * @param string aProp
   *        The property you want to get the descriptor for.
   * @return object
   *         Property descriptor.
   */
  getPropertyDescriptor: function WCU_getPropertyDescriptor(aObject, aProp)
  {
    let desc = null;
    while (aObject) {
      try {
        if ((desc = Object.getOwnPropertyDescriptor(aObject, aProp))) {
          break;
        }
      }
      catch (ex if (ex.name == "NS_ERROR_XPC_BAD_CONVERT_JS" ||
                    ex.name == "NS_ERROR_XPC_BAD_OP_ON_WN_PROTO" ||
                    ex.name == "TypeError")) {
        // Native getters throw here. See bug 520882.
        // null throws TypeError.
      }
      try {
        aObject = Object.getPrototypeOf(aObject);
      }
      catch (ex if (ex.name == "TypeError")) {
        return desc;
      }
    }
    return desc;
  },

  /**
   * Sort function for object properties.
   *
   * @param object a
   *        Property descriptor.
   * @param object b
   *        Property descriptor.
   * @return integer
   *         -1 if a.name < b.name,
   *         1 if a.name > b.name,
   *         0 otherwise.
   */
  propertiesSort: function WCU_propertiesSort(a, b)
  {
    // Convert the pair.name to a number for later sorting.
    let aNumber = parseFloat(a.name);
    let bNumber = parseFloat(b.name);

    // Sort numbers.
    if (!isNaN(aNumber) && isNaN(bNumber)) {
      return -1;
    }
    else if (isNaN(aNumber) && !isNaN(bNumber)) {
      return 1;
    }
    else if (!isNaN(aNumber) && !isNaN(bNumber)) {
      return aNumber - bNumber;
    }
    // Sort string.
    else if (a.name < b.name) {
      return -1;
    }
    else if (a.name > b.name) {
      return 1;
    }
    else {
      return 0;
    }
  },

  /**
   * Create a grip for the given value. If the value is an object,
   * an object wrapper will be created.
   *
   * @param mixed aValue
   *        The value you want to create a grip for, before sending it to the
   *        client.
   * @param function aObjectWrapper
   *        If the value is an object then the aObjectWrapper function is
   *        invoked to give us an object grip. See this.getObjectGrip().
   * @return mixed
   *         The value grip.
   */
  createValueGrip: function WCU_createValueGrip(aValue, aObjectWrapper)
  {
    switch (typeof aValue) {
      case "boolean":
        return aValue;
      case "string":
        return aObjectWrapper(aValue);
      case "number":
        if (aValue === Infinity) {
          return { type: "Infinity" };
        }
        else if (aValue === -Infinity) {
          return { type: "-Infinity" };
        }
        else if (Number.isNaN(aValue)) {
          return { type: "NaN" };
        }
        else if (!aValue && 1 / aValue === -Infinity) {
          return { type: "-0" };
        }
        return aValue;
      case "undefined":
        return { type: "undefined" };
      case "object":
        if (aValue === null) {
          return { type: "null" };
        }
      case "function":
        return aObjectWrapper(aValue);
      default:
        Cu.reportError("Failed to provide a grip for value of " + typeof aValue
                       + ": " + aValue);
        return null;
    }
  },

  /**
   * Check if the given object is an iterator or a generator.
   *
   * @param object aObject
   *        The object you want to check.
   * @return boolean
   *         True if the given object is an iterator or a generator, otherwise
   *         false is returned.
   */
  isIteratorOrGenerator: function WCU_isIteratorOrGenerator(aObject)
  {
    if (aObject === null) {
      return false;
    }

    if (typeof aObject == "object") {
      if (typeof aObject.__iterator__ == "function" ||
          aObject.constructor && aObject.constructor.name == "Iterator") {
        return true;
      }

      try {
        let str = aObject.toString();
        if (typeof aObject.next == "function" &&
            str.indexOf("[object Generator") == 0) {
          return true;
        }
      }
      catch (ex) {
        // window.history.next throws in the typeof check above.
        return false;
      }
    }

    return false;
  },

  /**
   * Determine if the given request mixes HTTP with HTTPS content.
   *
   * @param string aRequest
   *        Location of the requested content.
   * @param string aLocation
   *        Location of the current page.
   * @return boolean
   *         True if the content is mixed, false if not.
   */
  isMixedHTTPSRequest: function WCU_isMixedHTTPSRequest(aRequest, aLocation)
  {
    try {
      let requestURI = Services.io.newURI(aRequest, null, null);
      let contentURI = Services.io.newURI(aLocation, null, null);
      return (contentURI.scheme == "https" && requestURI.scheme != "https");
    }
    catch (ex) {
      return false;
    }
  },

  /**
   * Helper function to deduce the name of the provided function.
   *
   * @param funtion aFunction
   *        The function whose name will be returned.
   * @return string
   *         Function name.
   */
  getFunctionName: function WCF_getFunctionName(aFunction)
  {
    let name = null;
    if (aFunction.name) {
      name = aFunction.name;
    }
    else {
      let desc;
      try {
        desc = aFunction.getOwnPropertyDescriptor("displayName");
      }
      catch (ex) { }
      if (desc && typeof desc.value == "string") {
        name = desc.value;
      }
    }
    if (!name) {
      try {
        let str = (aFunction.toString() || aFunction.toSource()) + "";
        name = (str.match(REGEX_MATCH_FUNCTION_NAME) || [])[1];
      }
      catch (ex) { }
    }
    return name;
  },

  /**
   * Get the object class name. For example, the |window| object has the Window
   * class name (based on [object Window]).
   *
   * @param object aObject
   *        The object you want to get the class name for.
   * @return string
   *         The object class name.
   */
  getObjectClassName: function WCU_getObjectClassName(aObject)
  {
    if (aObject === null) {
      return "null";
    }
    if (aObject === undefined) {
      return "undefined";
    }

    let type = typeof aObject;
    if (type != "object") {
      // Grip class names should start with an uppercase letter.
      return type.charAt(0).toUpperCase() + type.substr(1);
    }

    let className;

    try {
      className = ((aObject + "").match(/^\[object (\S+)\]$/) || [])[1];
      if (!className) {
        className = ((aObject.constructor + "").match(/^\[object (\S+)\]$/) || [])[1];
      }
      if (!className && typeof aObject.constructor == "function") {
        className = this.getFunctionName(aObject.constructor);
      }
    }
    catch (ex) { }

    return className;
  },

  /**
   * Check if the given value is a grip with an actor.
   *
   * @param mixed aGrip
   *        Value you want to check if it is a grip with an actor.
   * @return boolean
   *         True if the given value is a grip with an actor.
   */
  isActorGrip: function WCU_isActorGrip(aGrip)
  {
    return aGrip && typeof(aGrip) == "object" && aGrip.actor;
  },
};
exports.Utils = WebConsoleUtils;

//////////////////////////////////////////////////////////////////////////
// Localization
//////////////////////////////////////////////////////////////////////////

WebConsoleUtils.l10n = function WCU_l10n(aBundleURI)
{
  this._bundleUri = aBundleURI;
};

WebConsoleUtils.l10n.prototype = {
  _stringBundle: null,

  get stringBundle()
  {
    if (!this._stringBundle) {
      this._stringBundle = Services.strings.createBundle(this._bundleUri);
    }
    return this._stringBundle;
  },

  /**
   * Generates a formatted timestamp string for displaying in console messages.
   *
   * @param integer [aMilliseconds]
   *        Optional, allows you to specify the timestamp in milliseconds since
   *        the UNIX epoch.
   * @return string
   *         The timestamp formatted for display.
   */
  timestampString: function WCU_l10n_timestampString(aMilliseconds)
  {
    let d = new Date(aMilliseconds ? aMilliseconds : null);
    let hours = d.getHours(), minutes = d.getMinutes();
    let seconds = d.getSeconds(), milliseconds = d.getMilliseconds();
    let parameters = [hours, minutes, seconds, milliseconds];
    return this.getFormatStr("timestampFormat", parameters);
  },

  /**
   * Retrieve a localized string.
   *
   * @param string aName
   *        The string name you want from the Web Console string bundle.
   * @return string
   *         The localized string.
   */
  getStr: function WCU_l10n_getStr(aName)
  {
    let result;
    try {
      result = this.stringBundle.GetStringFromName(aName);
    }
    catch (ex) {
      Cu.reportError("Failed to get string: " + aName);
      throw ex;
    }
    return result;
  },

  /**
   * Retrieve a localized string formatted with values coming from the given
   * array.
   *
   * @param string aName
   *        The string name you want from the Web Console string bundle.
   * @param array aArray
   *        The array of values you want in the formatted string.
   * @return string
   *         The formatted local string.
   */
  getFormatStr: function WCU_l10n_getFormatStr(aName, aArray)
  {
    let result;
    try {
      result = this.stringBundle.formatStringFromName(aName, aArray, aArray.length);
    }
    catch (ex) {
      Cu.reportError("Failed to format string: " + aName);
      throw ex;
    }
    return result;
  },
};


//////////////////////////////////////////////////////////////////////////
// JS Completer
//////////////////////////////////////////////////////////////////////////

(function _JSPP(WCU) {
const STATE_NORMAL = 0;
const STATE_QUOTE = 2;
const STATE_DQUOTE = 3;

const OPEN_BODY = "{[(".split("");
const CLOSE_BODY = "}])".split("");
const OPEN_CLOSE_BODY = {
  "{": "}",
  "[": "]",
  "(": ")",
};

const MAX_COMPLETIONS = 1500;

/**
 * Analyses a given string to find the last statement that is interesting for
 * later completion.
 *
 * @param   string aStr
 *          A string to analyse.
 *
 * @returns object
 *          If there was an error in the string detected, then a object like
 *
 *            { err: "ErrorMesssage" }
 *
 *          is returned, otherwise a object like
 *
 *            {
 *              state: STATE_NORMAL|STATE_QUOTE|STATE_DQUOTE,
 *              startPos: index of where the last statement begins
 *            }
 */
function findCompletionBeginning(aStr)
{
  let bodyStack = [];

  let state = STATE_NORMAL;
  let start = 0;
  let c;
  for (let i = 0; i < aStr.length; i++) {
    c = aStr[i];

    switch (state) {
      // Normal JS state.
      case STATE_NORMAL:
        if (c == '"') {
          state = STATE_DQUOTE;
        }
        else if (c == "'") {
          state = STATE_QUOTE;
        }
        else if (c == ";") {
          start = i + 1;
        }
        else if (c == " ") {
          start = i + 1;
        }
        else if (OPEN_BODY.indexOf(c) != -1) {
          bodyStack.push({
            token: c,
            start: start
          });
          start = i + 1;
        }
        else if (CLOSE_BODY.indexOf(c) != -1) {
          var last = bodyStack.pop();
          if (!last || OPEN_CLOSE_BODY[last.token] != c) {
            return {
              err: "syntax error"
            };
          }
          if (c == "}") {
            start = i + 1;
          }
          else {
            start = last.start;
          }
        }
        break;

      // Double quote state > " <
      case STATE_DQUOTE:
        if (c == "\\") {
          i++;
        }
        else if (c == "\n") {
          return {
            err: "unterminated string literal"
          };
        }
        else if (c == '"') {
          state = STATE_NORMAL;
        }
        break;

      // Single quote state > ' <
      case STATE_QUOTE:
        if (c == "\\") {
          i++;
        }
        else if (c == "\n") {
          return {
            err: "unterminated string literal"
          };
        }
        else if (c == "'") {
          state = STATE_NORMAL;
        }
        break;
    }
  }

  return {
    state: state,
    startPos: start
  };
}

/**
 * Provides a list of properties, that are possible matches based on the passed
 * Debugger.Environment/Debugger.Object and inputValue.
 *
 * @param object aDbgObject
 *        When the debugger is not paused this Debugger.Object wraps the scope for autocompletion.
 *        It is null if the debugger is paused.
 * @param object anEnvironment
 *        When the debugger is paused this Debugger.Environment is the scope for autocompletion.
 *        It is null if the debugger is not paused.
 * @param string aInputValue
 *        Value that should be completed.
 * @param number [aCursor=aInputValue.length]
 *        Optional offset in the input where the cursor is located. If this is
 *        omitted then the cursor is assumed to be at the end of the input
 *        value.
 * @returns null or object
 *          If no completion valued could be computed, null is returned,
 *          otherwise a object with the following form is returned:
 *            {
 *              matches: [ string, string, string ],
 *              matchProp: Last part of the inputValue that was used to find
 *                         the matches-strings.
 *            }
 */
function JSPropertyProvider(aDbgObject, anEnvironment, aInputValue, aCursor)
{
  if (aCursor === undefined) {
    aCursor = aInputValue.length;
  }

  let inputValue = aInputValue.substring(0, aCursor);

  // Analyse the inputValue and find the beginning of the last part that
  // should be completed.
  let beginning = findCompletionBeginning(inputValue);

  // There was an error analysing the string.
  if (beginning.err) {
    return null;
  }

  // If the current state is not STATE_NORMAL, then we are inside of an string
  // which means that no completion is possible.
  if (beginning.state != STATE_NORMAL) {
    return null;
  }

  let completionPart = inputValue.substring(beginning.startPos);

  // Don't complete on just an empty string.
  if (completionPart.trim() == "") {
    return null;
  }

  let lastDot = completionPart.lastIndexOf(".");
  if (lastDot > 0 &&
      (completionPart[0] == "'" || completionPart[0] == '"') &&
      completionPart[lastDot - 1] == completionPart[0]) {
    // We are completing a string literal.
    let matchProp = completionPart.slice(lastDot + 1);
    return getMatchedProps(String.prototype, matchProp);
  }

  // We are completing a variable / a property lookup.
  let properties = completionPart.split(".");
  let matchProp = properties.pop().trimLeft();
  let obj = aDbgObject;

  // The first property must be found in the environment if the debugger is
  // paused.
  if (anEnvironment) {
    if (properties.length == 0) {
      return getMatchedPropsInEnvironment(anEnvironment, matchProp);
    }
    obj = getVariableInEnvironment(anEnvironment, properties.shift());
  }

  if (!isObjectUsable(obj)) {
    return null;
  }

  // We get the rest of the properties recursively starting from the Debugger.Object
  // that wraps the first property
  for (let prop of properties) {
    prop = prop.trim();
    if (!prop) {
      return null;
    }

    if (/\[\d+\]$/.test(prop))??{
      // The property to autocomplete is a member of array. For example
      // list[i][j]..[n]. Traverse the array to get the actual element.
      obj = getArrayMemberProperty(obj, prop);
    }
    else {
      obj = DevToolsUtils.getProperty(obj, prop);
    }

    if (!isObjectUsable(obj)) {
      return null;
    }
  }

  // If the final property is a primitive
  if (typeof obj != "object") {
    return getMatchedProps(obj, matchProp);
  }

  return getMatchedPropsInDbgObject(obj, matchProp);
}

/**
 * Get the array member of aObj for the given aProp. For example, given
 * aProp='list[0][1]' the element at [0][1] of aObj.list is returned.
 *
 * @param object aObj
 *        The object to operate on.
 * @param string aProp
 *        The property to return.
 * @return null or Object
 *         Returns null if the property couldn't be located. Otherwise the array
 *         member identified by aProp.
 */
function getArrayMemberProperty(aObj, aProp)
{
  // First get the array.
  let obj = aObj;
  let propWithoutIndices = aProp.substr(0, aProp.indexOf("["));
  obj = DevToolsUtils.getProperty(obj, propWithoutIndices);
  if (!isObjectUsable(obj)) {
    return null;
  }

  // Then traverse the list of indices to get the actual element.
  let result;
  let arrayIndicesRegex = /\[[^\]]*\]/g;
  while ((result = arrayIndicesRegex.exec(aProp)) !== null) {
    let indexWithBrackets = result[0];
    let indexAsText = indexWithBrackets.substr(1, indexWithBrackets.length - 2);
    let index = parseInt(indexAsText);

    if (isNaN(index)) {
      return null;
    }

    obj = DevToolsUtils.getProperty(obj, index);

    if (!isObjectUsable(obj)) {
      return null;
    }
  }

  return obj;
}

/**
 * Check if the given Debugger.Object can be used for autocomplete.
 *
 * @param Debugger.Object aObject
 *        The Debugger.Object to check.
 * @return boolean
 *         True if further inspection into the object is possible, or false
 *         otherwise.
 */
function isObjectUsable(aObject)
{
  if (aObject == null) {
    return false;
  }

  if (typeof aObject == "object" && aObject.class == "DeadObject") {
    return false;
  }

  return true;
}

/**
 * @see getExactMatch_impl()
 */
function getVariableInEnvironment(anEnvironment, aName)
{
  return getExactMatch_impl(anEnvironment, aName, DebuggerEnvironmentSupport);
}

/**
 * @see getMatchedProps_impl()
 */
function getMatchedPropsInEnvironment(anEnvironment, aMatch)
{
  return getMatchedProps_impl(anEnvironment, aMatch, DebuggerEnvironmentSupport);
}

/**
 * @see getMatchedProps_impl()
 */
function getMatchedPropsInDbgObject(aDbgObject, aMatch)
{
  return getMatchedProps_impl(aDbgObject, aMatch, DebuggerObjectSupport);
}

/**
 * @see getMatchedProps_impl()
 */
function getMatchedProps(aObj, aMatch)
{
  if (typeof aObj != "object") {
    aObj = aObj.constructor.prototype;
  }
  return getMatchedProps_impl(aObj, aMatch, JSObjectSupport);
}

/**
 * Get all properties in the given object (and its parent prototype chain) that
 * match a given prefix.
 *
 * @param mixed aObj
 *        Object whose properties we want to filter.
 * @param string aMatch
 *        Filter for properties that match this string.
 * @return object
 *         Object that contains the matchProp and the list of names.
 */
function getMatchedProps_impl(aObj, aMatch, {chainIterator, getProperties})
{
  let matches = new Set();

  // We need to go up the prototype chain.
  let iter = chainIterator(aObj);
  for (let obj of iter) {
    let props = getProperties(obj);
    for (let prop of props) {
      if (prop.indexOf(aMatch) != 0) {
        continue;
      }

      // If it is an array index, we can't take it.
      // This uses a trick: converting a string to a number yields NaN if
      // the operation failed, and NaN is not equal to itself.
      if (+prop != +prop) {
        matches.add(prop);
      }

      if (matches.size > MAX_COMPLETIONS) {
        break;
      }
    }

    if (matches.size > MAX_COMPLETIONS) {
      break;
    }
  }

  return {
    matchProp: aMatch,
    matches: [...matches],
  };
}

/**
 * Returns a property value based on its name from the given object, by
 * recursively checking the object's prototype.
 *
 * @param object aObj
 *        An object to look the property into.
 * @param string aName
 *        The property that is looked up.
 * @returns object|undefined
 *        A Debugger.Object if the property exists in the object's prototype
 *        chain, undefined otherwise.
 */
function getExactMatch_impl(aObj, aName, {chainIterator, getProperty})
{
  // We need to go up the prototype chain.
  let iter = chainIterator(aObj);
  for (let obj of iter) {
    let prop = getProperty(obj, aName, aObj);
    if (prop) {
      return prop.value;
    }
  }
  return undefined;
}


let JSObjectSupport = {
  chainIterator: function(aObj)
  {
    while (aObj) {
      yield aObj;
      aObj = Object.getPrototypeOf(aObj);
    }
  },

  getProperties: function(aObj)
  {
    return Object.getOwnPropertyNames(aObj);
  },

  getProperty: function()
  {
    // getProperty is unsafe with raw JS objects.
    throw "Unimplemented!";
  },
};

let DebuggerObjectSupport = {
  chainIterator: function(aObj)
  {
    while (aObj) {
      yield aObj;
      aObj = aObj.proto;
    }
  },

  getProperties: function(aObj)
  {
    return aObj.getOwnPropertyNames();
  },

  getProperty: function(aObj, aName, aRootObj)
  {
    // This is left unimplemented in favor to DevToolsUtils.getProperty().
    throw "Unimplemented!";
  },
};

let DebuggerEnvironmentSupport = {
  chainIterator: function(aObj)
  {
    while (aObj) {
      yield aObj;
      aObj = aObj.parent;
    }
  },

  getProperties: function(aObj)
  {
    return aObj.names();
  },

  getProperty: function(aObj, aName)
  {
    // TODO: we should use getVariableDescriptor() here - bug 725815.
    let result = aObj.getVariable(aName);
    // FIXME: Need actual UI, bug 941287.
    if (result.optimizedOut || result.missingArguments) {
      return null;
    }
    return result === undefined ? null : { value: result };
  },
};


exports.JSPropertyProvider = DevToolsUtils.makeInfallible(JSPropertyProvider);
})(WebConsoleUtils);

///////////////////////////////////////////////////////////////////////////////
// The page errors listener
///////////////////////////////////////////////////////////////////////////////

/**
 * The nsIConsoleService listener. This is used to send all of the console
 * messages (JavaScript, CSS and more) to the remote Web Console instance.
 *
 * @constructor
 * @param nsIDOMWindow [aWindow]
 *        Optional - the window object for which we are created. This is used
 *        for filtering out messages that belong to other windows.
 * @param object aListener
 *        The listener object must have one method:
 *        - onConsoleServiceMessage(). This method is invoked with one argument,
 *        the nsIConsoleMessage, whenever a relevant message is received.
 */
function ConsoleServiceListener(aWindow, aListener)
{
  this.window = aWindow;
  this.listener = aListener;
  if (this.window) {
    this.layoutHelpers = new LayoutHelpers(this.window);
  }
}
exports.ConsoleServiceListener = ConsoleServiceListener;

ConsoleServiceListener.prototype =
{
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIConsoleListener]),

  /**
   * The content window for which we listen to page errors.
   * @type nsIDOMWindow
   */
  window: null,

  /**
   * The listener object which is notified of messages from the console service.
   * @type object
   */
  listener: null,

  /**
   * Initialize the nsIConsoleService listener.
   */
  init: function CSL_init()
  {
    Services.console.registerListener(this);
  },

  /**
   * The nsIConsoleService observer. This method takes all the script error
   * messages belonging to the current window and sends them to the remote Web
   * Console instance.
   *
   * @param nsIConsoleMessage aMessage
   *        The message object coming from the nsIConsoleService.
   */
  observe: function CSL_observe(aMessage)
  {
    if (!this.listener) {
      return;
    }

    if (this.window) {
      if (!(aMessage instanceof Ci.nsIScriptError) ||
          !aMessage.outerWindowID ||
          !this.isCategoryAllowed(aMessage.category)) {
        return;
      }

      let errorWindow = Services.wm.getOuterWindowWithId(aMessage.outerWindowID);
      if (!errorWindow || !this.layoutHelpers.isIncludedInTopLevelWindow(errorWindow)) {
        return;
      }
    }

    this.listener.onConsoleServiceMessage(aMessage);
  },

  /**
   * Check if the given message category is allowed to be tracked or not.
   * We ignore chrome-originating errors as we only care about content.
   *
   * @param string aCategory
   *        The message category you want to check.
   * @return boolean
   *         True if the category is allowed to be logged, false otherwise.
   */
  isCategoryAllowed: function CSL_isCategoryAllowed(aCategory)
  {
    if (!aCategory) {
      return false;
    }

    switch (aCategory) {
      case "XPConnect JavaScript":
      case "component javascript":
      case "chrome javascript":
      case "chrome registration":
      case "XBL":
      case "XBL Prototype Handler":
      case "XBL Content Sink":
      case "xbl javascript":
        return false;
    }

    return true;
  },

  /**
   * Get the cached page errors for the current inner window and its (i)frames.
   *
   * @param boolean [aIncludePrivate=false]
   *        Tells if you want to also retrieve messages coming from private
   *        windows. Defaults to false.
   * @return array
   *         The array of cached messages. Each element is an nsIScriptError or
   *         an nsIConsoleMessage
   */
  getCachedMessages: function CSL_getCachedMessages(aIncludePrivate = false)
  {
    let errors = Services.console.getMessageArray() || [];

    // if !this.window, we're in a browser console. Still need to filter
    // private messages.
    if (!this.window) {
      return errors.filter((aError) => {
        if (aError instanceof Ci.nsIScriptError) {
          if (!aIncludePrivate && aError.isFromPrivateWindow) {
            return false;
          }
        }

        return true;
      });
    }

    let ids = WebConsoleUtils.getInnerWindowIDsForFrames(this.window);

    return errors.filter((aError) => {
      if (aError instanceof Ci.nsIScriptError) {
        if (!aIncludePrivate && aError.isFromPrivateWindow) {
          return false;
        }
        if (ids &&
            (ids.indexOf(aError.innerWindowID) == -1 ||
             !this.isCategoryAllowed(aError.category))) {
          return false;
        }
      }
      else if (ids && ids[0]) {
        // If this is not an nsIScriptError and we need to do window-based
        // filtering we skip this message.
        return false;
      }

      return true;
    });
  },

  /**
   * Remove the nsIConsoleService listener.
   */
  destroy: function CSL_destroy()
  {
    Services.console.unregisterListener(this);
    this.listener = this.window = null;
  },
};


///////////////////////////////////////////////////////////////////////////////
// The window.console API observer
///////////////////////////////////////////////////////////////////////////////

/**
 * The window.console API observer. This allows the window.console API messages
 * to be sent to the remote Web Console instance.
 *
 * @constructor
 * @param nsIDOMWindow aWindow
 *        Optional - the window object for which we are created. This is used
 *        for filtering out messages that belong to other windows.
 * @param object aOwner
 *        The owner object must have the following methods:
 *        - onConsoleAPICall(). This method is invoked with one argument, the
 *        Console API message that comes from the observer service, whenever
 *        a relevant console API call is received.
 */
function ConsoleAPIListener(aWindow, aOwner)
{
  this.window = aWindow;
  this.owner = aOwner;
  if (this.window) {
    this.layoutHelpers = new LayoutHelpers(this.window);
  }
}
exports.ConsoleAPIListener = ConsoleAPIListener;

ConsoleAPIListener.prototype =
{
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIObserver]),

  /**
   * The content window for which we listen to window.console API calls.
   * @type nsIDOMWindow
   */
  window: null,

  /**
   * The owner object which is notified of window.console API calls. It must
   * have a onConsoleAPICall method which is invoked with one argument: the
   * console API call object that comes from the observer service.
   *
   * @type object
   * @see WebConsoleActor
   */
  owner: null,

  /**
   * Initialize the window.console API observer.
   */
  init: function CAL_init()
  {
    // Note that the observer is process-wide. We will filter the messages as
    // needed, see CAL_observe().
    Services.obs.addObserver(this, "console-api-log-event", false);
  },

  /**
   * The console API message observer. When messages are received from the
   * observer service we forward them to the remote Web Console instance.
   *
   * @param object aMessage
   *        The message object receives from the observer service.
   * @param string aTopic
   *        The message topic received from the observer service.
   */
  observe: function CAL_observe(aMessage, aTopic)
  {
    if (!this.owner) {
      return;
    }

    let apiMessage = aMessage.wrappedJSObject;
    if (this.window) {
      let msgWindow = Services.wm.getCurrentInnerWindowWithId(apiMessage.innerID);
      if (!msgWindow || !this.layoutHelpers.isIncludedInTopLevelWindow(msgWindow)) {
        // Not the same window!
        return;
      }
    }

    this.owner.onConsoleAPICall(apiMessage);
  },

  /**
   * Get the cached messages for the current inner window and its (i)frames.
   *
   * @param boolean [aIncludePrivate=false]
   *        Tells if you want to also retrieve messages coming from private
   *        windows. Defaults to false.
   * @return array
   *         The array of cached messages.
   */
  getCachedMessages: function CAL_getCachedMessages(aIncludePrivate = false)
  {
    let messages = [];
    let ConsoleAPIStorage = Cc["@mozilla.org/consoleAPI-storage;1"]
                              .getService(Ci.nsIConsoleAPIStorage);

    // if !this.window, we're in a browser console. Retrieve all events
    // for filtering based on privacy.
    if (!this.window) {
      messages = ConsoleAPIStorage.getEvents();
    } else {
      let ids = WebConsoleUtils.getInnerWindowIDsForFrames(this.window);
      ids.forEach((id) => {
        messages = messages.concat(ConsoleAPIStorage.getEvents(id));
      });
    }

    if (aIncludePrivate) {
      return messages;
    }

    return messages.filter((m) => !m.private);
  },

  /**
   * Destroy the console API listener.
   */
  destroy: function CAL_destroy()
  {
    Services.obs.removeObserver(this, "console-api-log-event");
    this.window = this.owner = null;
  },
};



/**
 * JSTerm helper functions.
 *
 * Defines a set of functions ("helper functions") that are available from the
 * Web Console but not from the web page.
 *
 * A list of helper functions used by Firebug can be found here:
 *   http://getfirebug.com/wiki/index.php/Command_Line_API
 *
 * @param object aOwner
 *        The owning object.
 */
function JSTermHelpers(aOwner)
{
  /**
   * Find a node by ID.
   *
   * @param string aId
   *        The ID of the element you want.
   * @return nsIDOMNode or null
   *         The result of calling document.querySelector(aSelector).
   */
  aOwner.sandbox.$ = function JSTH_$(aSelector)
  {
    return aOwner.window.document.querySelector(aSelector);
  };

  /**
   * Find the nodes matching a CSS selector.
   *
   * @param string aSelector
   *        A string that is passed to window.document.querySelectorAll.
   * @return nsIDOMNodeList
   *         Returns the result of document.querySelectorAll(aSelector).
   */
  aOwner.sandbox.$$ = function JSTH_$$(aSelector)
  {
    return aOwner.window.document.querySelectorAll(aSelector);
  };

  /**
   * Runs an xPath query and returns all matched nodes.
   *
   * @param string aXPath
   *        xPath search query to execute.
   * @param [optional] nsIDOMNode aContext
   *        Context to run the xPath query on. Uses window.document if not set.
   * @return array of nsIDOMNode
   */
  aOwner.sandbox.$x = function JSTH_$x(aXPath, aContext)
  {
    let nodes = new aOwner.window.wrappedJSObject.Array();
    let doc = aOwner.window.document;
    aContext = aContext || doc;

    let results = doc.evaluate(aXPath, aContext, null,
                               Ci.nsIDOMXPathResult.ANY_TYPE, null);
    let node;
    while ((node = results.iterateNext())) {
      nodes.push(node);
    }

    return nodes;
  };

  /**
   * Returns the currently selected object in the highlighter.
   *
   * TODO: this implementation crosses the client/server boundaries! This is not
   * usable within a remote browser. To implement this feature correctly we need
   * support for remote inspection capabilities within the Inspector as well.
   * See bug 787975.
   *
   * @return nsIDOMElement|null
   *         The DOM element currently selected in the highlighter.
   */
   Object.defineProperty(aOwner.sandbox, "$0", {
    get: function() {
      let window = aOwner.chromeWindow();
      if (!window) {
        return null;
      }

      let target = null;
      try {
        target = devtools.TargetFactory.forTab(window.gBrowser.selectedTab);
      }
      catch (ex) {
        // If we report this exception the user will get it in the Browser
        // Console every time when she evaluates any string.
      }

      if (!target) {
        return null;
      }

      let toolbox = gDevTools.getToolbox(target);
      let node = toolbox && toolbox.selection ? toolbox.selection.node : null;

      return node ? aOwner.makeDebuggeeValue(node) : null;
    },
    enumerable: true,
    configurable: false
  });

  /**
   * Clears the output of the JSTerm.
   */
  aOwner.sandbox.clear = function JSTH_clear()
  {
    aOwner.helperResult = {
      type: "clearOutput",
    };
  };

  /**
   * Returns the result of Object.keys(aObject).
   *
   * @param object aObject
   *        Object to return the property names from.
   * @return array of strings
   */
  aOwner.sandbox.keys = function JSTH_keys(aObject)
  {
    return aOwner.window.wrappedJSObject.Object.keys(WebConsoleUtils.unwrap(aObject));
  };

  /**
   * Returns the values of all properties on aObject.
   *
   * @param object aObject
   *        Object to display the values from.
   * @return array of string
   */
  aOwner.sandbox.values = function JSTH_values(aObject)
  {
    let arrValues = new aOwner.window.wrappedJSObject.Array();
    let obj = WebConsoleUtils.unwrap(aObject);

    for (let prop in obj) {
      arrValues.push(obj[prop]);
    }

    return arrValues;
  };

  /**
   * Opens a help window in MDN.
   */
  aOwner.sandbox.help = function JSTH_help()
  {
    aOwner.helperResult = { type: "help" };
  };

  /**
   * Change the JS evaluation scope.
   *
   * @param DOMElement|string|window aWindow
   *        The window object to use for eval scope. This can be a string that
   *        is used to perform document.querySelector(), to find the iframe that
   *        you want to cd() to. A DOMElement can be given as well, the
   *        .contentWindow property is used. Lastly, you can directly pass
   *        a window object. If you call cd() with no arguments, the current
   *        eval scope is cleared back to its default (the top window).
   */
  aOwner.sandbox.cd = function JSTH_cd(aWindow)
  {
    if (!aWindow) {
      aOwner.consoleActor.evalWindow = null;
      aOwner.helperResult = { type: "cd" };
      return;
    }

    if (typeof aWindow == "string") {
      aWindow = aOwner.window.document.querySelector(aWindow);
    }
    if (aWindow instanceof Ci.nsIDOMElement && aWindow.contentWindow) {
      aWindow = aWindow.contentWindow;
    }
    if (!(aWindow instanceof Ci.nsIDOMWindow)) {
      aOwner.helperResult = { type: "error", message: "cdFunctionInvalidArgument" };
      return;
    }

    aOwner.consoleActor.evalWindow = aWindow;
    aOwner.helperResult = { type: "cd" };
  };

  /**
   * Inspects the passed aObject. This is done by opening the PropertyPanel.
   *
   * @param object aObject
   *        Object to inspect.
   */
  aOwner.sandbox.inspect = function JSTH_inspect(aObject)
  {
    let dbgObj = aOwner.makeDebuggeeValue(aObject);
    let grip = aOwner.createValueGrip(dbgObj);
    aOwner.helperResult = {
      type: "inspectObject",
      input: aOwner.evalInput,
      object: grip,
    };
  };

  /**
   * Prints aObject to the output.
   *
   * @param object aObject
   *        Object to print to the output.
   * @return string
   */
  aOwner.sandbox.pprint = function JSTH_pprint(aObject)
  {
    if (aObject === null || aObject === undefined || aObject === true ||
        aObject === false) {
      aOwner.helperResult = {
        type: "error",
        message: "helperFuncUnsupportedTypeError",
      };
      return null;
    }

    aOwner.helperResult = { rawOutput: true };

    if (typeof aObject == "function") {
      return aObject + "\n";
    }

    let output = [];

    let obj = WebConsoleUtils.unwrap(aObject);
    for (let name in obj) {
      let desc = WebConsoleUtils.getPropertyDescriptor(obj, name) || {};
      if (desc.get || desc.set) {
        // TODO: Bug 842672 - toolkit/ imports modules from browser/.
        let getGrip = VariablesView.getGrip(desc.get);
        let setGrip = VariablesView.getGrip(desc.set);
        let getString = VariablesView.getString(getGrip);
        let setString = VariablesView.getString(setGrip);
        output.push(name + ":", "  get: " + getString, "  set: " + setString);
      }
      else {
        let valueGrip = VariablesView.getGrip(obj[name]);
        let valueString = VariablesView.getString(valueGrip);
        output.push(name + ": " + valueString);
      }
    }

    return "  " + output.join("\n  ");
  };

  /**
   * Print a string to the output, as-is.
   *
   * @param string aString
   *        A string you want to output.
   * @return void
   */
  aOwner.sandbox.print = function JSTH_print(aString)
  {
    aOwner.helperResult = { rawOutput: true };
    return String(aString);
  };
}
exports.JSTermHelpers = JSTermHelpers;


/**
 * A ReflowObserver that listens for reflow events from the page.
 * Implements nsIReflowObserver.
 *
 * @constructor
 * @param object aWindow
 *        The window for which we need to track reflow.
 * @param object aOwner
 *        The listener owner which needs to implement:
 *        - onReflowActivity(aReflowInfo)
 */

function ConsoleReflowListener(aWindow, aListener)
{
  this.docshell = aWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                         .getInterface(Ci.nsIWebNavigation)
                         .QueryInterface(Ci.nsIDocShell);
  this.listener = aListener;
  this.docshell.addWeakReflowObserver(this);
}

exports.ConsoleReflowListener = ConsoleReflowListener;

ConsoleReflowListener.prototype =
{
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIReflowObserver,
                                         Ci.nsISupportsWeakReference]),
  docshell: null,
  listener: null,

  /**
   * Forward reflow event to listener.
   *
   * @param DOMHighResTimeStamp aStart
   * @param DOMHighResTimeStamp aEnd
   * @param boolean aInterruptible
   */
  sendReflow: function CRL_sendReflow(aStart, aEnd, aInterruptible)
  {
    let frame = components.stack.caller.caller;

    let filename = frame.filename;

    if (filename) {
      // Because filename could be of the form "xxx.js -> xxx.js -> xxx.js",
      // we only take the last part.
      filename = filename.split(" ").pop();
    }

    this.listener.onReflowActivity({
      interruptible: aInterruptible,
      start: aStart,
      end: aEnd,
      sourceURL: filename,
      sourceLine: frame.lineNumber,
      functionName: frame.name
    });
  },

  /**
   * On uninterruptible reflow
   *
   * @param DOMHighResTimeStamp aStart
   * @param DOMHighResTimeStamp aEnd
   */
  reflow: function CRL_reflow(aStart, aEnd)
  {
    this.sendReflow(aStart, aEnd, false);
  },

  /**
   * On interruptible reflow
   *
   * @param DOMHighResTimeStamp aStart
   * @param DOMHighResTimeStamp aEnd
   */
  reflowInterruptible: function CRL_reflowInterruptible(aStart, aEnd)
  {
    this.sendReflow(aStart, aEnd, true);
  },

  /**
   * Unregister listener.
   */
  destroy: function CRL_destroy()
  {
    this.docshell.removeWeakReflowObserver(this);
    this.listener = this.docshell = null;
  },
};

function gSequenceId()
{
  return gSequenceId.n++;
}
gSequenceId.n = 0;
