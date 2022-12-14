const {Cc,Ci,Cu} = require("chrome");
const ObservableObject = require("devtools/shared/observable-object");
const promise = require("devtools/toolkit/deprecated-sync-thenables");

const {EventEmitter} = Cu.import("resource://gre/modules/devtools/event-emitter.js");
const {generateUUID} = Cc['@mozilla.org/uuid-generator;1'].getService(Ci.nsIUUIDGenerator);

/**
 * IndexedDB wrapper that just save project objects
 *
 * The only constraint is that project objects have to have
 * a unique `location` object.
 */

const global = this;
const IDB = {
  _db: null,

  open: function () {
    let deferred = promise.defer();

    var idbManager = Cc["@mozilla.org/dom/indexeddb/manager;1"]
                       .getService(Ci.nsIIndexedDatabaseManager);
    idbManager.initWindowless(global);

    let request = global.indexedDB.open("AppProjects", 5);
    request.onerror = function(event) {
      deferred.reject("Unable to open AppProjects indexedDB. " +
                      "Error code: " + event.target.errorCode);
    };
    request.onupgradeneeded = function(event) {
      let db = event.target.result;
      db.createObjectStore("projects", { keyPath: "location" });
    };

    request.onsuccess = function() {
      let db = IDB._db = request.result;
      let objectStore = db.transaction("projects").objectStore("projects");
      let projects = []
      objectStore.openCursor().onsuccess = function(event) {
        let cursor = event.target.result;
        if (cursor) {
          if (cursor.value.location) {
            // We need to make sure this object has a `.location` property.
            // The UI depends on this property.
            // This should not be needed as we make sure to register valid
            // projects, but in the past (before bug 924568), we might have
            // registered invalid objects.
            projects.push(cursor.value);
          }
          cursor.continue();
        } else {
          deferred.resolve(projects);
        }
      };
    };

    return deferred.promise;
  },

  add: function(project) {
    let deferred = promise.defer();

    project = JSON.parse(JSON.stringify(project));

    if (!project.location) {
      // We need to make sure this object has a `.location` property.
      deferred.reject("Missing location property on project object.");
    } else {
      let transaction = IDB._db.transaction(["projects"], "readwrite");
      let objectStore = transaction.objectStore("projects");
      let request = objectStore.add(project);
      request.onerror = function(event) {
        deferred.reject("Unable to add project to the AppProjects indexedDB: " +
                        this.error.name + " - " + this.error.message );
      };
      request.onsuccess = function() {
        deferred.resolve();
      };
    }

    return deferred.promise;
  },

  update: function(project) {
    let deferred = promise.defer();

    var transaction = IDB._db.transaction(["projects"], "readwrite");
    var objectStore = transaction.objectStore("projects");
    var request = objectStore.put(project);
    request.onerror = function(event) {
      deferred.reject("Unable to update project to the AppProjects indexedDB: " +
                      this.error.name + " - " + this.error.message );
    };
    request.onsuccess = function() {
      deferred.resolve();
    };

    return deferred.promise;
  },

  remove: function(location) {
    let deferred = promise.defer();

    let request = IDB._db.transaction(["projects"], "readwrite")
                    .objectStore("projects")
                    .delete(location);
    request.onsuccess = function(event) {
      deferred.resolve();
    };
    request.onerror = function() {
      deferred.reject("Unable to delete project to the AppProjects indexedDB: " +
                      this.error.name + " - " + this.error.message );
    };

    return deferred.promise;
  }
};

const store = new ObservableObject({ projects:[] });

let loadDeferred = promise.defer();

IDB.open().then(function (projects) {
  store.object.projects = projects;
  AppProjects.emit("ready", store.object.projects);
  loadDeferred.resolve();
});

const AppProjects = {
  load: function() {
    return loadDeferred.promise;
  },

  addPackaged: function(folder) {
    let project = {
      type: "packaged",
      location: folder.path,
      // We need a unique id, that is the app origin,
      // in order to identify the app when being installed on the device.
      // The packaged app local path is a valid id, but only on the client.
      // This origin will be used to generate the true id of an app:
      // its manifest URL.
      // If the app ends up specifying an explicit origin in its manifest,
      // we will override this random UUID on app install.
      packagedAppOrigin: generateUUID().toString().slice(1, -1)
    };
    return IDB.add(project).then(function () {
      store.object.projects.push(project);
      // return the added objects (proxified)
      return store.object.projects[store.object.projects.length - 1];
    });
  },

  addHosted: function(manifestURL) {
    let project = {
      type: "hosted",
      location: manifestURL
    };
    return IDB.add(project).then(function () {
      store.object.projects.push(project);
      // return the added objects (proxified)
      return store.object.projects[store.object.projects.length - 1];
    });
  },

  update: function (project) {
    return IDB.update({
      type: project.type,
      location: project.location,
      packagedAppOrigin: project.packagedAppOrigin
    }).then(() => project);
  },

  remove: function(location) {
    return IDB.remove(location).then(function () {
      let projects = store.object.projects;
      for (let i = 0; i < projects.length; i++) {
        if (projects[i].location == location) {
          projects.splice(i, 1);
          return;
        }
      }
      throw new Error("Unable to find project in AppProjects store");
    });
  },

  get: function(location) {
    let projects = store.object.projects;
    for (let i = 0; i < projects.length; i++) {
      if (projects[i].location == location) {
        return projects[i];
      }
    }
    return null;
  },

  store: store
};

EventEmitter.decorate(AppProjects);

exports.AppProjects = AppProjects;

