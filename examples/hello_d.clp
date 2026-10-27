(deftemplate greeting (slot who))
(defrule hello
  (greeting (who ?x))
  =>
  (printout t "Hello, " ?x "!" crlf))
