window.addEventListener("load", function () {
  var context = document.getElementById("game").getContext("2d");
  console.log("game.js ran, canvas " + (context ? "2d" : "none") + ", localStorage " + typeof window.localStorage);
});
